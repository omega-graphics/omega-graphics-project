=====================
Autom Language Syntax
=====================

Autom uses an expressive, object-oriented language similar to the syntax of Python.

There are 2 file types used in Autom. ( AUTOM.build and \*.autom)

==========


--------
Keywords
--------
**var**
    Defines a variable

    Usage::

        var myVar = ["foo","bar"]

**func**
    Defines a function

    Usage::

        var testVar = "Hello World"
        func myFunc(param) {
            print(msg:param)
        }

        myFunc(param:testVar)

**import**
    Imports \*.autom file with the corresenponding name.

    Usage::

        import "./autom/myModule"

**if**
    Defines the start of a conditional collection as well as a first conditional test.

    Usage::

        if(toolchain.name == "LLVM"){
            print(msg:"Using LLVM Toolchain")
        }

**elif**
    Defines of an alternative conditonal test in an existing collection.
**else**
    Defines the end of a conditional collection.

**foreach**
    Iterates over the elements of an array.

    Usage::

        var items = ["a", "b", "c"]
        foreach item in items {
            print(msg:item)
        }

--------------------------
Builtin Variables/Objects:
--------------------------

**autom**
    An object interface for accessing internal values
    such as the cfamily toolchain or the target build system.

    *Properties:*
        ```.toolchain```
            The name of the cfamily toolchain that was selected by AUTOM.
        
        ```.c_flags```
            The default C compiler flags to use 

        ```.cxx_flags```
            The default CXX compiler flags to use.

------------------
Object Properties:
------------------

**Target Properties**
    
    ```.name```
        The name of the target used by AUTOM internal.
        NOTE: Each target declared must have a unique name.

**Compiled Target Properties**

    ```.output_name```
        The output filename of the compiled target.
        (This value is by default equal to the value set in ```.name```)
    
    ```.output_ext```
        The output filename extension of the compiled target.
        (This value is by default equal to the standard output file extension of the compiled target type with the corresponding target system.
        For example, a Shared target compiled for aarch64-darwin will have an output extension dylib)

        Default Values on each Target OS :
            
            Windows --> .exe (Executable), .lib (Static), .dll (Shared)

            Darwin  --> .a (Static), .dylib (Shared)

            Linux   --> .a (Static), .so (Shared)

    ```.include_dirs```   
        The extra directories to search for include files. (Headers)

    ```.libs```
        The extra libraries to link to the compiled target.
    
    ```.lib_dirs```
        The extra directories to search for linkable libraries.

**Target Config Properties**
    Every Target has there own



------------------
Builtin Functions:
------------------

**print(msg:any) -> Void**
    Prints a value to the console.

    Usage::

        print(msg:"Hello World")
        # --> "Hello World"

        print(msg:["foo","bar"])
        # --> ["foo","bar"]

        print(msg:true)
        # --> true

**project(name:string, version:string) -> Void**
    Declares the current project. Must be called before any targets are created.

    Usage::

        project(name:"MyLib", version:"1.0")

**subdir(path:string) -> Void**
    Evaluates the ``AUTOM.build`` file found in the given subdirectory.
    The subdirectory path is relative to the current build file.

    Usage::

        subdir(path:"./engine")

**configure(in:string, out:string) -> Void**
    Reads the input file, substitutes all ``@VAR@`` tokens with the value of
    the corresponding variable in the current scope, and writes the result to
    the output file. The output directory is created automatically if it does
    not exist.

    Substitution rules:

    - ``@IDENT@`` is replaced by the string value of variable ``IDENT``.
    - Lookup is case-sensitive and matches the exact variable name.
    - A bare ``@`` not followed by a valid identifier (letters, digits, ``_``)
      is passed through literally, so email addresses and similar text are safe.
    - If ``IDENT`` is not defined in the current scope, or is not a ``String``,
      the build stops with an error and the output file is removed.

    Usage::

        var APP_VERSION = "1.2.0"
        configure(in:"./src/version.h.in", out:"./gen/version.h")

        # version.h.in contains:
        #   #define APP_VERSION "@APP_VERSION@"
        # version.h output:
        #   #define APP_VERSION "1.2.0"

**find_program(cmd:string) -> string**
    Searches ``PATH`` for the given program name and returns its absolute path,
    or ``Void`` if the program is not found.

    Usage::

        var cmake_path = find_program(cmd:"cmake")

**Executable(name:string, sources:string[]) -> Executable**
    Creates an executable target.

**Shared(name:string, sources:string[]) -> Shared**
    Creates a shared library target.

**Archive(name:string, sources:string[]) -> Archive**
    Creates a static library target (produces a ``.a`` / ``.lib`` archive).

**SourceGroup(name:string, sources:string[]) -> SourceGroup**
    Creates a source group — a named collection of source files that can be
    linked into other targets as a dependency but does not produce its own
    output binary.

**GroupTarget(name:string, deps:string[]) -> GroupTarget**
    Creates a group target that aggregates other named targets under a single
    dependency name. Useful for expressing "build all of these together."

    Usage::

        GroupTarget(name:"all_libs", deps:["engine", "renderer"])

**Script(name:string, cmd:string, args:string[], outputs:string[]) -> Script**
    Creates a custom script target. ``cmd`` is the program to invoke,
    ``args`` are the arguments passed to it, and ``outputs`` lists the files
    the script produces. At least one output must be declared.

    Usage::

        Script(
            name:"gen_header",
            cmd:"python3",
            args:["./tools/gen.py", "--out", "./gen/header.h"],
            outputs:["./gen/header.h"]
        )

**Copy(name:string, sources:string[], dest:string) -> Copy**
    Creates a target that copies one or more files or directories to ``dest``.

**Symlink(name:string, source:string, dest:string) -> Symlink**
    Creates a target that creates a symbolic link at ``dest`` pointing to
    ``source``.

**Mkdir(name:string, dest:string) -> Mkdir**
    Creates a target that creates the directory at ``dest``.

**install_targets(targets:string[], dest:string) -> Void**
    Registers one or more targets for installation to the given destination
    prefix. Records an entry in the ``AUTOMINSTALL`` file.

**install_files(files:string[], dest:string) -> Void**
    Registers one or more files for installation to the given destination
    prefix. Records an entry in the ``AUTOMINSTALL`` file.






