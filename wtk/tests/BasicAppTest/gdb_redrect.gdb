# GDB script to debug red rect not visible.
# Run from project root:
#   gdb -x wtk/tests/BasicAppTest/gdb_redrect.gdb --args ./build/wtk/tests/BasicAppTest

set environment LD_LIBRARY_PATH=/home/alex/Documents/Git/omega-graphics-project/build/lib:/home/alex/Documents/Git/omega-graphics-project/build/wtk/deps/icu/lib:/home/alex/Documents/Git/omega-graphics-project/build/wtk/deps/libpng/lib:/home/alex/Documents/Git/omega-graphics-project/build/wtk/deps/libjpeg-turbo/lib:/home/alex/Documents/Git/omega-graphics-project/build/wtk/deps/libtiff/lib

# Break in onPaint after ensureUIView (line 46) - confirms uiView was created and bounds are valid
break BasicAppTestRun.cpp:46
commands
  silent
  printf "onPaint bounds: w=%.1f h=%.1f uiView=%p\n", bounds.w, bounds.h, uiView.get()
  continue
end

# Break after setLayout (line 65) - confirms redRect coords
break BasicAppTestRun.cpp:65
commands
  silent
  printf "redRect: x=%.1f y=%.1f w=%.1f h=%.1f\n", redRect.pos.x, redRect.pos.y, redRect.w, redRect.h
  continue
end

# Break after uiView->update() to confirm we reach the end of onPaint
break BasicAppTestRun.cpp:72
commands
  silent
  printf "uiView->update() called\n"
  continue
end

run
