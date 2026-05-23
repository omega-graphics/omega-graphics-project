==========
About AQUA
==========

.. contents:: On this page
   :local:
   :depth: 2

----

What AQUA Is
------------

AQUA is the physics / simulation engine of the Omega Graphics suite — the
simulation counterpart to OmegaGTE (graphics). Omega kREATE, the 3D game
engine, consumes AQUA for collision and dynamics the same way it consumes
OmegaGTE for rendering.

Like OmegaGTE, AQUA keeps its backend hidden behind its public API. Callers
work with ``AQSpace``, ``AQRigidBody``, and ``AQBodyDesc``; they never depend
on how a step is actually computed. That boundary is what lets the execution
strategy below evolve — from a CPU placeholder integrator today to GPU compute
dispatch later — without any consumer of AQUA changing a line.

----

AQContext and the Command Queue
-------------------------------

``AQContext`` is the central class for every physics operation in AQUA. It is
constructed from a single dependency — an OmegaGTE ``GECommandQueue`` — and that
command queue is the channel through which all simulation work reaches the
device:

.. code-block:: cpp

   auto context = AQContext::Create(commandQueue);

Holding the command queue at the context level, rather than threading a device
or queue through every call, means the simulation has one well-defined place to
record and submit work. A physics step becomes a sequence of commands encoded
onto that queue and committed — exactly the model OmegaGTE already uses for
rendering, compute, and blit. AQUA does not invent a second submission path; it
shares the graphics engine's command queue so that physics and rendering are
scheduled through the same primitive.

The context is also the owner of physics state and time. Simulation spaces are
created through ``AQContext::createSpace`` and retained by the context, and the
context keeps simulation time with a **fixed-timestep accumulator**: callers
feed real elapsed frame time to ``AQContext::advance(realDt)``, and the context
runs as many fixed-size sub-steps as fit, banking the surplus for the next call.
This decouples the simulation rate from the caller's frame rate and keeps
stepping deterministic — the same configuration of bodies advances identically
regardless of how the frame timing happens to land. Individual spaces are not
stepped directly; all timekeeping lives in one place.

----

Compute Dispatch, With a Software Fallback
------------------------------------------

The simulation work AQUA performs — integration, broad-phase and narrow-phase
collision, constraint solving — is data-parallel: the same operation applied
across many bodies or contacts. That is the shape compute kernels exist for.

AQUA's intended execution model is therefore to **dispatch physics to compute
kernels** through the OmegaGTE command queue held by ``AQContext``. Because
OmegaGTE compiles a single OmegaSL kernel to Direct3D 12, Metal, and Vulkan
compute, AQUA inherits GPU acceleration on every backend OmegaGTE supports
rather than on one vendor's hardware.

Not every device can run that dispatch. Older hardware, headless or virtualized
environments, and devices whose OmegaGTE backend reports no usable compute
capability all need a path that does not depend on the GPU. For those, AQUA
provides a **software fallback** — a CPU implementation of the same simulation
step that produces equivalent results, selected automatically when compute
dispatch is unavailable. The choice is made by querying the device's
capabilities, not by a compile-time platform fork, mirroring how OmegaGTE gates
features through ``GTEDeviceFeatures``.

The public API is identical in both cases. A caller advancing the simulation
with ``AQContext::advance(realDt)`` does not know, and does not need to know,
whether the work ran on the GPU or on the CPU fallback.

.. note::

   **Status — early scaffold.** Today ``AQSpace`` advances bodies with a
   placeholder semi-implicit Euler integrator on the CPU (gravity only, no
   collision). The compute-kernel dispatch path and the production CPU solver
   described above are the planned architecture, not the current state. See
   ``kreate/docs/Engine-Roadmap.md`` (Phase 8) for how physics slots into the
   engine and the build-vs-integrate decision that lives here.

----

Why Compute Dispatch Instead of CUDA
------------------------------------

The dominant GPU physics engine, NVIDIA **PhysX**, accelerates its simulation
through **CUDA**. CUDA is NVIDIA's proprietary compute platform, and it runs
natively only on NVIDIA GPUs. PhysX's GPU solver requires an NVIDIA GPU of
compute capability SM 6.0 (Pascal) or newer; when no compatible NVIDIA device
is present, PhysX does not GPU-accelerate at all — it runs an entirely separate
CPU solver instead. There is no GPU-accelerated PhysX on AMD, Intel, or Apple
hardware.

Two clarifications on the common framing of this:

* The non-NVIDIA path is a **distinct CPU implementation**, not an emulation of
  CUDA. PhysX does not attempt to run its CUDA kernels in software; it falls
  back to code written for the CPU. Running actual CUDA on non-NVIDIA GPUs only
  exists through third-party reimplementations such as ZLUDA, which are not part
  of PhysX.
* The practical consequence the comparison cares about still holds: outside
  NVIDIA hardware, PhysX gets **no GPU acceleration** — every other GPU sits
  idle while the CPU does the work.

AQUA takes the opposite approach by building on OmegaGTE's compute abstraction.
A physics kernel is authored once in OmegaSL and compiled to D3D12, Metal, and
Vulkan compute, so the *GPU* path — not just a CPU fallback — is available on
any modern GPU: AMD, Intel, Apple Silicon, and NVIDIA alike. AQUA's CPU fallback
exists for devices with no usable compute support at all, the same role PhysX's
CPU solver plays — but for AQUA it is the exception for incapable hardware,
whereas for PhysX it is the *only* option on the majority of non-NVIDIA GPUs.

This mirrors the philosophy of OmegaGTE itself: cross-platform GPU capability is
part of the contract, expressed through one API and one shader source, rather
than tied to a single vendor's compute stack.

----

The Role of AQUA in the Omega Project
-------------------------------------

AQUA sits alongside OmegaGTE as a foundation layer, consumed by Omega kREATE:

* **OmegaGTE** provides the device, command queue, and compute pipelines.
* **AQUA** uses an OmegaGTE command queue, by way of ``AQContext``, to run
  simulation — GPU compute where available, CPU fallback otherwise.
* **kREATE** drives both: it renders scenes through OmegaGTE and simulates
  collision and dynamics through AQUA, sharing the same underlying device.

Keeping AQUA's backend behind its public API is what makes that division clean.
kREATE depends on what AQUA *does*, never on how it computes a step.
