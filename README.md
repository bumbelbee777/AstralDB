# AstralDB - A blazingly fast, lightweight, relational DB in modern C++

AstralDB is a high-performant yet lightweight and relatively secure relational SQL database implemented in modern C++.

Demo:
[Placeholder Item]

**How is it so fast?** AstralDB leverages non-blocking asynchronous multithreading thanks to modern C++ language features (coroutines, `std::async`, `std::jthread`, `std::stop_token`, etc...), aggressive caching/prefetching of data, lightweight synchronization data structures, and its overall minimalist design philosophy. This is coupled with the custom optimizing JIT for SQL that optimizes Query before running it.