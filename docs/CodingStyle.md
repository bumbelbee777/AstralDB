# AstralDB Coding Style (C/C++)

## Overview

AstralDB's coding style is oriented towards simplicity and efficiency. While being conscious of performance and security. Of course, balancing these requirements and demands isn't the most straightforward thing, hence why this piece of documentation outlines not just the coding style, but best practices when it comes to C/C++ code as well.

## Styling

- Use tabs for ident (with tab size of 4).
- Always keep the first brace up.
- Single-line declarations are fine, just don't make them overly long.
- No ident for namespaces.

**Example:**
```cpp
namespace Global {
class Foo {
	int HiddenVar;
public:
	Foo(int HiddenVar) : HiddenVar(HiddenVar) {} //This is a comment.

	/*Pretend this is a multi-line comment explaining some stuff
	Lorem ipsum dolor sit amet...*/
	int Bar() {
		if(HiddenVar > 0) return HiddenVar + 2;
		else if(HiddenVar = 0) return HiddenVar - 2;
		return HiddenVar - 2;
	}
};
}
```

## Naming Conventions

- Use PascalCase for variables, functions, and classes.
- Using UPPER_SNAKE_CASE for certain constants is fine, don't overuse it though.
- Names must be meaningful and clear, making the code self-explanatory and eliminating the need for excessive comments.
- For setters, don't prefix them with `GetX()`, instead, they should just be `X()`.

**Example:**
```cpp
static constexpr int VERY_USEFUL_CONSTANT = 69420;

class SomeClass {
	int X_; //Good
	int Y; //Bad
public:
	SomeClass() = default;

	int GetY() const { return X_; } //Bad
	int X() const { return X_; } //Good

	void SetY(int NewY) const { Y = NewY; }
	void SetX(int NewX) { X_ = X; } //Good
};
```

## Language Features

- Avoid raw pointers whenever possible, use Xenon abstractions like those found in `SafePointersNeStatus.hxx` (`UniquePointer<T>`, `SharedPointer<T>`, `SmartPointer<T>`, `WeakPointer<T>`) alongside built-in language features (`reinterpret_cast`, `const_cast`, `static_cast`, etc..) to work with pointers and memory in general.
- Same goes with the preprocessor (outside of includes and simple definitions), for more complex or hacky things use C++ templates instead, which are more powerful and robust.
- Since we'll mostly be working in kernel-mode and generally low-level environments, use `NeStatus` for return environments in the kernel. For Xenon, use standard data types (`void`, `int`, `bool`, etc...).
- Don't use exceptions except for user-space components, instead use assertions and error messages/panics if possible.
- When working with pointers and references, **ALWAYS** make sure to clean up and/or dereference properly.

## Best Practices
- Avoid `new` and `delete` whenever possible, use smart pointers or overloaded operators instead.
- Only include necessary headers to avoid slower compile times and other complications.
- Whenever possible, use forward declarations and keep track of dependencies across components and files to avoid cyclic dependencies.
- Source files (`.cpp`) are for implementations and header files (`NeStatus.hxx`) are for definitions and light implementations/utilities.
- For speed, prioritize CPU registers over the stack, and the stack over the heap.
- Avoid unnecessary locks and try keeping code as lock-free as possible (not at the expense of wacky workarounds, though).
- When it comes to functions, make sure to not make them ridiculously long, messy, and complicated. If you find yourself writing a long function, consider breaking it down into multiple smaller ones to make management easier (especially if some logic recurs in other pieces of code).
- Since compilers may tamper with inline assembly in an undesirable manner, it is best to use `volatile` to prevent compiler optimizations, `"memory"` clobbers to prevent reordering, listing registers as clobbers, using extended assembly, and using `[[gnu::naked]]` for full control over functions written in assembly or the `Nooptimize` macro to disable optimizations entirely.
