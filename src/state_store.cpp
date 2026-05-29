#include "state_store.hpp"

#include <modmesh/serialization/SerializableItem.hpp>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace modmesh_bot
{

namespace
{

// The MM_DECL_SERIALIZABLE macro expands unqualified `detail::JsonNode` etc.,
// so we alias modmesh::detail into the enclosing namespace for lookup.
namespace detail = modmesh::detail;

struct StateFile : modmesh::SerializableItem
{
    std::vector<int> reviewed_prs;
    std::vector<std::int64_t> handled_comments;
    std::string cursor_updated_at;
    std::int64_t cursor_id = 0;

    MM_DECL_SERIALIZABLE(
        register_member("reviewed_prs", reviewed_prs);
        register_member("handled_comments", handled_comments);
        register_member("cursor_updated_at", cursor_updated_at);
        register_member("cursor_id", cursor_id);)
};

std::string read_all(int fd)
{
    std::string out;
    char buf[4096];
    for (;;)
    {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0)
        {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "read state file");
        }
        out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
}

std::string parent_dir(const std::string & path)
{
    auto slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

void fsync_dir(const std::string & dir)
{
    int dfd = ::open(dir.c_str(), O_RDONLY);
    if (dfd < 0) return; // best effort
    ::fsync(dfd);
    ::close(dfd);
}

} // namespace

StateStore::StateStore(std::string path)
    : path_(std::move(path))
    , lock_path_(path_ + ".lock")
{
    // Lock a separate file we hold for the bot's lifetime. The state file
    // itself is rewritten via rename(), which would orphan a lock taken on
    // the file we open here.
    lock_fd_ = ::open(lock_path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd_ < 0)
    {
        throw std::system_error(errno, std::generic_category(),
            "open lock file: " + lock_path_);
    }

    if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0)
    {
        int err = errno;
        ::close(lock_fd_);
        lock_fd_ = -1;
        if (err == EWOULDBLOCK)
        {
            throw std::runtime_error(
                "another modmesh-bot instance holds the state lock: " + lock_path_);
        }
        throw std::system_error(err, std::generic_category(),
            "flock lock file: " + lock_path_);
    }

    try
    {
        load();
    }
    catch (...)
    {
        ::flock(lock_fd_, LOCK_UN);
        ::close(lock_fd_);
        lock_fd_ = -1;
        throw;
    }
}

StateStore::~StateStore()
{
    if (lock_fd_ >= 0)
    {
        ::flock(lock_fd_, LOCK_UN);
        ::close(lock_fd_);
    }
}

void StateStore::load()
{
    int fd = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        if (errno == ENOENT) return; // first run
        throw std::system_error(errno, std::generic_category(),
            "open state file: " + path_);
    }
    std::string body;
    try
    {
        body = read_all(fd);
    }
    catch (...)
    {
        ::close(fd);
        throw;
    }
    ::close(fd);
    if (body.empty()) return;

    StateFile sf;
    sf.from_json(body);

    reviewed_prs_.insert(sf.reviewed_prs.begin(), sf.reviewed_prs.end());
    handled_comments_.insert(sf.handled_comments.begin(), sf.handled_comments.end());
    cursor_updated_at_ = std::move(sf.cursor_updated_at);
    cursor_id_ = sf.cursor_id;
}

bool StateStore::reviewed(int pr) const
{
    return reviewed_prs_.find(pr) != reviewed_prs_.end();
}

void StateStore::mark_reviewed(int pr)
{
    reviewed_prs_.insert(pr);
}

bool StateStore::handled(std::int64_t comment_id) const
{
    return handled_comments_.find(comment_id) != handled_comments_.end();
}

void StateStore::mark_handled(std::int64_t comment_id)
{
    handled_comments_.insert(comment_id);
}

void StateStore::advance_cursor(const std::string & updated_at, std::int64_t comment_id)
{
    if (updated_at > cursor_updated_at_
        || (updated_at == cursor_updated_at_ && comment_id > cursor_id_))
    {
        cursor_updated_at_ = updated_at;
        cursor_id_ = comment_id;
    }
}

bool StateStore::is_at_or_before_cursor(const std::string & updated_at, std::int64_t comment_id) const
{
    if (updated_at < cursor_updated_at_) return true;
    if (updated_at > cursor_updated_at_) return false;
    return comment_id <= cursor_id_;
}

void StateStore::save()
{
    StateFile sf;
    sf.reviewed_prs.assign(reviewed_prs_.begin(), reviewed_prs_.end());
    sf.handled_comments.assign(handled_comments_.begin(), handled_comments_.end());
    sf.cursor_updated_at = cursor_updated_at_;
    sf.cursor_id = cursor_id_;
    std::string json = sf.to_json();

    std::string tmp = path_ + ".tmp";
    int tfd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (tfd < 0)
    {
        throw std::system_error(errno, std::generic_category(), "open tmp state file: " + tmp);
    }

    const char * p = json.data();
    std::size_t remaining = json.size();
    while (remaining > 0)
    {
        ssize_t n = ::write(tfd, p, remaining);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            int err = errno;
            ::close(tfd);
            ::unlink(tmp.c_str());
            throw std::system_error(err, std::generic_category(), "write tmp state file");
        }
        p += n;
        remaining -= static_cast<std::size_t>(n);
    }

    if (::fsync(tfd) != 0)
    {
        int err = errno;
        ::close(tfd);
        ::unlink(tmp.c_str());
        throw std::system_error(err, std::generic_category(), "fsync tmp state file");
    }
    ::close(tfd);

    if (::rename(tmp.c_str(), path_.c_str()) != 0)
    {
        int err = errno;
        ::unlink(tmp.c_str());
        throw std::system_error(err, std::generic_category(), "rename tmp state file");
    }

    fsync_dir(parent_dir(path_));
}

} // namespace modmesh_bot
