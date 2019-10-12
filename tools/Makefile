CPPFLAGS=-I ../src/ -DNDEBUG
CXXFLAGS=`pkg-config --cflags fftw3f sndfile samplerate` -Wall
LOADLIBES=`pkg-config --libs fftw3f sndfile samplerate` -lm

fftgain: fftgain.cc ../src/audiosrc.cc
