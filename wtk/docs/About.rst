===================
About OmegaWTK
===================

.. contents:: On this page
   :local:
   :depth: 2

----

The Problem with Cross-Platform UI
------------------------------------

Building a cross-platform desktop application should be a question of product
design: what does the application do, how does it look, and how does it behave?
In practice, a significant fraction of the work goes into the toolkit itself —
learning its API, working around its constraints, and writing code that exists
only to satisfy the framework rather than to express the application.

Modern development compounds this. AI-assisted coding reduces the time to write
boilerplate, but boilerplate still occupies context space, still appears in
diffs, still needs to be reviewed, and still couples the application to toolkit
abstractions that provide no direct user value. The goal is not to write
boilerplate faster. It is to stop writing it.

OmegaWTK exists because the available cross-platform UI options each impose
costs that are not necessary.

----

Why Not Qt?
------------

Qt is the most established cross-platform C++ UI toolkit and has a large
ecosystem. It is also the product of an era when the constraints were
different — desktop-first, single-threaded event loops, and a rendering model
built before GPU-accelerated compositing became standard.

The practical problems for a new project:

**Licensing cost.** Qt's commercial license is required for proprietary
applications. For independent developers and small teams, this is a real
barrier. The LGPLv3 option is available but carries its own constraints around
dynamic linking and modification disclosure that many commercial projects
cannot accept without legal review.

**Age and API surface.** Qt has been accumulating API since 1992. The result
is a toolkit where multiple solutions exist for the same problem — often a
deprecated one and a current one — and where understanding which patterns are
current requires knowing the history of what was superseded and when. The
signal-slot mechanism, moc, and the QObject hierarchy are clever solutions to
problems that modern C++ has since addressed at the language level.

**Declining adoption.** Qt's dominance in the C++ desktop UI space has eroded
as Electron and web-based toolkits captured the majority of new cross-platform
application development. The commercial trajectory reflects this: Qt has
restructured its licensing model multiple times in recent years. Building a
new system on a toolkit whose ecosystem is contracting is a compounding
technical risk.

----

Why Not Electron / CEF?
------------------------

Electron and applications built on the Chromium Embedded Framework (CEF)
represent the dominant model for cross-platform desktop applications today.
The appeal is clear: web technologies are universal, the talent pool is large,
and the browser's layout and rendering engine handles an enormous amount of
complexity automatically.

The costs are structural:

**Runtime size.** A minimal Electron application ships the entire Chromium
browser runtime — renderer process, V8, the full web platform stack. The
resulting install size typically exceeds 200 MB before any application code is
included. For a utility application or developer tool, that overhead exists
entirely to support a rendering model the application did not choose; it was
inherited with the toolkit.

**Performance model.** Chromium's rendering architecture was designed for the
threat model of a multi-origin browser: each origin gets a sandboxed renderer
process, communication crosses process boundaries via IPC, and the compositor
runs on a separate thread to keep scrolling smooth even when JavaScript is
busy. This is the right architecture for a browser. For a single-origin
application with no untrusted content, the process isolation adds IPC
overhead on every UI interaction without providing any benefit.

**HTML spec ceiling.** Customizing the visual appearance and interaction model
of an Electron application means working within what CSS and the HTML rendering
pipeline can express. Highly custom controls — non-rectangular widgets, GPU-
composited transitions, tight integration with native platform affordances —
require either complex CSS workarounds or dropping into native code, at which
point the cross-platform abstraction no longer applies uniformly.

**Language bridge cost.** Electron applications typically implement application
logic in JavaScript, with native modules bridging to system APIs. Every call
across the JavaScript/native boundary carries marshalling overhead. For UI
code that runs on every frame, this boundary is on the critical path.

----

What OmegaWTK Does Differently
---------------------------------

OmegaWTK is a C++ widget toolkit that renders entirely through OmegaGTE —
the same cross-platform GPU abstraction used by the rest of the Omega Graphics
Project. Every visible element is geometry triangulated and drawn via the
native GPU API for the platform: Direct3D 12 on Windows, Metal on macOS and
iOS, Vulkan on Linux and Android. There is no browser runtime, no intermediate
rendering engine, and no language bridge.

The design follows the same principle as OmegaGTE: remove the code that is
identical on every project and provides no user value. What remains is the
application.

**A single C++ API.** The same widget code compiles and runs on every
supported platform. Platform-specific behaviour — window management, input
routing, accessibility — is handled by the WTK backend, not by the
application.

**GPU-native rendering.** Widgets are drawn by the same render pipeline used
for 3D content. Compositing, animation, and custom visual effects are not
special cases requiring workarounds — they are first-class operations on GPU
resources.

**No runtime overhead.** There is no virtual machine, no IPC between renderer
and application processes, and no web platform stack. The application is the
process. The GPU receives draw commands directly.

**Small API footprint.** The public API covers what applications actually need:
layout, input handling, drawing, animation, and access to platform services.
It does not accumulate thirty years of superseded patterns.

The result is a toolkit that fits the development model of this era — where
the goal is to write as little structural code as possible and focus on what
the application actually does.

----
