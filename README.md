# AstralDB - A blazingly fast, lightweight, relational DB in modern C++

AstralDB is a high-performant yet lightweight and relatively secure relational SQL database implemented in modern C++.

Demo:
[Placeholder Item]

**How is it so fast?** AstralDB leverages non-blocking asynchronous multithreading thanks to modern C++ language features (coroutines, `std::async`, `std::jthread`, `std::stop_token`, etc...), aggressive caching/prefetching of data, lightweight synchronization primitives and lock-free, optimized data structures, and its overall minimalist design philosophy. This is coupled with the custom optimizing JIT for SQL that optimizes query before running it.

**What security does it have?** AstralDB leverages ASLR to randomize the database's address space alongside stack canaries to protect against stack smashing, and XChaCha20 to encrypt data before writing it to disk.