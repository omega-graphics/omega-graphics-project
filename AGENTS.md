# Coding Agents Info

## Code Style
    LLVM/Clang and Ninja for building on all platforms
        Use clang-format, clang-tidy to help cleanup and tidy the project files.
    Use PascalCase for file-naming conventions
    Use an object-oriented, Modular coding style that has modular rules so if a specific rule needs to be changed across the object it can be.

# Building



# Dependencies
    All dependencies in this project are pulled in via AUTOMDEPS. We often pull directly from the repos and build the source directly allowing us to control how third party deps are shipped with our APIs.

# Code Authoring
    All implementations will be revised through multi-phase plans before being implemented.
    (This is only for code, not for documentation, or other utilites)
   - 1 research existing/working idas solutions from other projects
   - 2 devise a new solution from those old solutions
   - 3 refine to include specific details about features/functionality
   - 4 write the multi-phase plan
   - 5 Implement Incrementally.

## Debugging
    It could either be a bug in the code or an architectural design flaw. If the code logic looks correct but fails to fix the issue, consider a design change to the system based on thorough, grounded research. (For a larger scoped issue that isn't resolving easily, the whole architecture of that region may need to be changed. However some bugs are very subtle, and are only due to the misuse of certain API's. For this, reasearch thoroughly and propose the cleanest patch.)
## Documenation
    All language written in any form of documentation (code comments, sphinx docs, READMEs) must be human readable.
- Code Comments (Doxygen):
    Use accurate, techincal language to describe the code details fully and correctly.
- User Guides (Sphinx docs) and README's:
    Use long-form prose that is clear and can be understood by someone with little to no technical experience. (The guides should be easy to follow and be able to explain how the code works without getting too techincal.)
