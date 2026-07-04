# User Guide

## Prerequisites

You have installed and compiled Ultrascan by following instructions in [Installation Guide](./installation_guide.md).

## Feature Enablement

Configure the `config.txt` file in the root directory of Ultrascan to enable or disable the short-byte-rule bypass optimization and false-positive blocking features. The features are enabled by default.

>![](public_sys-resources/icon-note.gif) **NOTE:**
>If the `config.txt` file is not included or the file format is incorrect, the features are disabled by default.

- `allowLily`: used to enable or disable the short-byte rule bypass optimization feature. The value can be `0` (disable) or `1` (enable).
- `allowNeoFdr`: used to enable or disable the false-positive blocking feature. The value can be `0` (disable) or `1` (enable).

The following is an example of the `config.txt` file:

```text
allowLily:1;allowNeoFdr:1;
```
