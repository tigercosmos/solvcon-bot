Project review guide for solvcon (github.com/solvcon/solvcon).

solvcon is a numerical PDE/CFD library. Its core container is SimpleArray, an
N-d array with ghost cells: `nghost` extends axis 0, whose logical index range
is [-nghost, shape(0) - nghost), not [0, shape(0)). The project's explicit goal
is NumPy parity for array semantics.

Probe every change that touches SimpleArray or array semantics against this
checklist — these are the failure modes reviewers find over and over:

- Ghost cells: does the code assume every axis starts at 0? Axis 0 does not
  when nghost > 0. Iteration must go through IndexRange / index_from_offset or
  handle the offset explicitly. Ask "what happens when nghost > 0" for every
  new loop over array elements.
- Empty and zero-size arrays: any dimension of size 0, and the 0-D (rank-zero)
  case. Loops written as do/while must not execute on an empty result. Check
  whether a rank-zero result is even representable before constructing it.
- Non-contiguous data: sliced, transposed, negative-stride, and F-order views.
  A fix that works on contiguous data may still be wrong or unsafe on views.
- Sibling and templated variants: when a function is fixed, check its
  overloads and templated siblings (e.g. a cross-type `reshape<U>()` next to
  `reshape()`) for the same defect. The diff fixing one variant while its twin
  keeps the bug is a recurring pattern here.
- NumPy parity: compare edge-case behavior (empty arrays, ties, NaN, error
  cases) against what numpy actually does, and say so explicitly.

Performance is a review concern, not an afterthought:

- Flag hot loops that replace linear contiguous traversal with bounds-checked
  access (`at()`) or runtime-rank index iteration. This class of change has
  produced multi-x regressions. Contiguous arrays should keep a direct linear
  fast path; the generic strided path is the fallback. Ask for a benchmark
  when a traversal strategy changes.

Test and documentation expectations in this project (these are requested in
almost every review here — treat them as substantive, not nits):

- Tests spell out expected values as literal constants; recomputing the
  expected value with the same expression as the implementation makes the
  test tautological and is rejected.
- Boolean behavior is asserted explicitly (assertTrue/assertFalse), not only
  via comparison with numpy output.
- Every new error message or exception path gets a test.
- User-visible semantics (ordering rules, edge-case behavior) get a concrete
  example in the documentation, not just prose.

Also flag code the change makes redundant: when a new mechanism (an offset
table, a helper) supersedes an existing computation elsewhere in the touched
files, point at the leftover duplicate.
