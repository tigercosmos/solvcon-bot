#pragma once

#include <cstdint>
#include <set>
#include <string>

namespace solvcon_bot
{

class StateStore
{
public:
    explicit StateStore(std::string path);
    ~StateStore();

    StateStore(const StateStore &) = delete;
    StateStore & operator=(const StateStore &) = delete;
    StateStore(StateStore &&) = delete;
    StateStore & operator=(StateStore &&) = delete;

    bool reviewed(int pr) const;
    void mark_reviewed(int pr);

    bool handled(std::int64_t comment_id) const;
    void mark_handled(std::int64_t comment_id);

    const std::string & cursor_updated_at() const { return cursor_updated_at_; }
    std::int64_t cursor_id() const { return cursor_id_; }
    void advance_cursor(const std::string & updated_at, std::int64_t comment_id);

    // True iff (updated_at, comment_id) <= persisted cursor.
    bool is_at_or_before_cursor(const std::string & updated_at, std::int64_t comment_id) const;

    void save();

private:
    void load();

    std::string path_;
    std::string lock_path_;
    int lock_fd_ = -1;

    std::set<int> reviewed_prs_;
    std::set<std::int64_t> handled_comments_;
    std::string cursor_updated_at_;
    std::int64_t cursor_id_ = 0;
};

} // namespace solvcon_bot
