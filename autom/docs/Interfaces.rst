==========
Interfaces
==========

Interfaces or Modules are files in AUTOM that can be used to define functions and variables for other build files to use.

Below are the standard interfaces that are provided with AUTOM and can be imported likewise.

----
"fs"
----

Provides filesystem utilities. Import with::

    import "fs"

| Functions:

**fs_abspath(path:string) -> string**

    Converts the given path to an absolute, lexically normalised path.

    Usage::

        var abs = fs_abspath(path:"./include")

**fs_exists(path:string) -> boolean**

    Returns ``true`` if the given path exists on the filesystem.

**fs_glob(path:string) -> string[]**

    Expands a glob pattern and returns the matching paths as an array of
    strings. Non-recursive (does not descend into subdirectories unless the
    pattern explicitly includes them).

    Usage::

        var srcs = fs_glob(path:"./src/*.cpp")

**fs_mkdir(path:string) -> Void**

    Creates the directory at ``path``, including any missing parent
    directories. Equivalent to ``mkdir -p``.

**fs_symlink(src:string, dest:string) -> Void**

    Creates a symbolic link at ``dest`` pointing to ``src``.

-------
"apple"
-------

Provides helpers for building Apple platform bundles. Import with::

    import "apple"

This module also imports ``"fs"`` automatically.

| Functions:

**AppleFramework(name:string, sources:string[]) -> Shared**

    Creates a ``.framework`` bundle for macOS. Builds a shared library,
    creates the bundle directory structure, copies headers, and code-signs
    the result.

    Returns the underlying ``Shared`` target so properties such as
    ``frameworks`` can be set on it directly.

    Usage::

        import "apple"

        var lib = AppleFramework(name:"MyLib", sources:fs_glob(path:"./src/*.cpp"))
        lib.frameworks = ["Cocoa.framework", "Metal.framework"]

**AppleApp(name:string, sources:string[]) -> Executable**

    Creates a ``.app`` bundle for macOS. Builds an executable, creates the
    bundle directory structure, and code-signs the result.

    Returns the underlying ``Executable`` target.

    Usage::

        import "apple"

        var app = AppleApp(name:"MyApp", sources:["./src/main.cpp"])

.. ----
.. "bridge"
.. ----
