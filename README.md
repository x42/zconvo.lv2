zeroconvo.lv2
=============

zeroconvolv2 is a [LV2](http://lv2plug.in) plugin to convolve audio signals
with zero configuration options: IRs are only available via presets.

Note that LV2 allows preset-bundles (many presets can be in a single bundle),
and a plugin can also have many of those preset-bundles.
(idea: "church reverb preset collection", "theatre reverb collection", etc.)


The plugin offers a framework for various IR sources (file, memory,
decoded virtual I/O).

The convolver adds one cycle (nominal block-size) latency and is able
to process any number of samples up to the nominal block-size, including
non-power-of-two blocksizes.

This plugin uses background processing and is suitable to process
long impulse-responses. Configurations up to true-stereo (4 channels)
are supported.

Install
-------

Don't. The plugin is stil work-in-progress.

```bash
make
#sudo make install PREFIX=/usr
ln -s "$(pwd)/build" ~/.lv2/zeroconvo.lv2
```
