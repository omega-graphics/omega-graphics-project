# MY_PROPOSALS

## Widget Resize Glitch Bug Fix

Propose full resizing tracking on the Window and Widget Delegate (For static renders, render should suspended until resize is finished. Measure acceleration and deceleration and a dead period to ascertain window/widget resize completion.) For animations, the problem is similar, but the compositor may have to get involved.

The compositor should measure a Render speed based off GPU load at that moment.
Velocity of window resize cannot exceed GPU capability at that moment.  (If it does, render packets will be dropped until the GPU is available again.)