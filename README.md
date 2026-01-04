# youtube2FITS
Convert youtube video to FITS file

Compilation and installation

C program, using gcc and cmake for compilation. Uses CFITSIO library.

```
mkdir build
cd build
cmake ..
make
```

Outputs a sequence of frames as a 3D FITS cube.

# Usage

Arguments:
- color channel(s): Which channel(s) should be used to create the greyscale output. R for red only, RGB for all.
- time sampleing [sec] of output file. One output frame is the average of input frames of this time sample
- youtube video

Example:
```
# use red channel only
# one frame for every 1.0 sec of video input
yt2FITS R 1.0 https://www.youtube.com/watch?v=Nqs0yBS3-JY vid.fits
```


