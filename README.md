zeroconvo.lv2
=============

zeroconvolv2 is a [LV2](http://lv2plug.in) plugin to convolve audio signals.

The convolution kernel is able to process any number of samples, up to the
nominal block-size, including non-power-of-two blocksizes.

It is intended for use with [Ardour 6](http://ardour.org), a LV2 host that
provides realtime-priority information to this plugin for efficient background
processing. Ardour also requires plugins to be able to handle an arbitrary number
of samples per cycle, up to the nominal block-size.

The plugin comes in two variants:

 * zero configuration options: IRs are only available via presets.
 * zero latency, user-loadable IRs with gain controls.

Note that LV2 allows preset-bundles (many presets can be in a single bundle),
and a plugin can also have many of those preset-bundles.
(idea: "church reverb preset collection", "theatre reverb collection", etc.)

Some presets are available from https://x42-plugins.com/x42/x42-zconvolver
These are all normalized to yield approx equal loudness, and tagged, so 
that Ardour 6 can show a handy preset selector.

This plugin uses background processing and is suitable to process
long impulse-responses. Configurations up to true-stereo (4 channels)
are supported.

The configurable convolver has the option to buffer the signal,
introducing one cycle of latency for increased reliability (and lower DSP
load. This is always enabled for the preset-variant.

For developers, the plugin also offers a framework for various IR
sources (file, memory, decoded virtual I/O). This can be used to
extend the plugin to process custom FIR, or to obfuscate/decrypt
IRs on demand.

Install
-------

```bash
make
#sudo make install PREFIX=/usr
ln -s "$(pwd)/build" ~/.lv2/zeroconvo.lv2
```
