# Complete AudioInputBuffer update

This package contains complete replacement files for the current
`work`-branch AudioSource/AudioInputBuffer implementation.

Replace these files in:

    lib/AudioLibrary/

- AudioSource.h
- AudioSourceFile.h
- AudioSourceFile.cpp
- AudioSourceStream.h
- AudioSourceStream.cpp
- AudioInputBuffer.h
- AudioInputBuffer.cpp

Also replace:

    test/test_audioinputbuffer_minimal.cpp

Delete the older:

    test/test_audioinputbuffer_prefetch.cpp

## Important local change

`AudioInputBuffer::reset()` is public in this version because the combined
Unity test uses it to switch from the file tests to the stream tests.

## Policy

Filesystem:
    threshold = 2048
    fillSize = 2048
    fillToThreshold = false

Streaming:
    threshold = 32768
    fillSize = 32768
    fillToThreshold = true

Therefore:

File:
    1536 -> request 2048 -> 3584

Stream:
    31744 -> request 1024 -> 32768

At or above the threshold:
    no read -> WOULD_BLOCK

The test contains 10 test cases.

Expected:
    10 passed, 0 failed
