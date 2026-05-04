=====
Utils
=====

`repo-stats`

A script that reports repo file counts, line counts, module SourceCode/deps/total disk usage, and repository root total size.


Building the Docs
=================

Omega Graphics uses two layers of documentation. 
Sphinx is used for our human readable guides to understanding each and every module in our project.
Our doxygen provides more detailed, direct API reference, as well as internal data structures.

The documentation is built using one script:

.. code-block:: bash

   ./utils/repo-docs 
   
This will output a folder entiled `doxygen` to the `build` folder.
There's also a project UML that can be generated using `repo-uml`.
(This is an experimental tool that we may end up phasing out and replacing with Doxygen generated UML)

`repo-uml`

