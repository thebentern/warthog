# Morse Micro Firmware Binaries

## Description

This component contains Morse Micro chip firmware binaries and board configuration files containing chip calibration
data, in the formats required for MM-IoT-SDK.

`mm6108/bcfs/bcf_HC01_V2_3V3.mbin` is Heltec's unmodified HC01 V2 calibration for a 3.3 V FEM.
It matches the calibration in the RadioCore's `libbcf_HC01_V2_L.a`.
The SHA-256 is `facb4831ea3752817736027efc646a8d180f0256d8539acb2a064e0c15fae597`.
[Vendor binary](https://resource.heltec.cn/download/HT-HC01_V2/BCF/driver_1_15_3/3.3V/bcf_HC01_V2_3V3.mbin) · [Heltec Arduino archive](https://github.com/HelTecAutomation/ESP_HaLow/blob/c36a4fdda94c2627b20a5ccd8cdca3a2b3601163/tools/bcf/libbcf_HC01_V2_L.a)

# License

The binary files contained within this package, are licensed under the Morse Micro Binary Distribution License Agreement,
available in the LICENSES/ directory contained in the root of this SDK.

Unless otherwise stated, source files and metadata contained within this component are provided under Apache 2.0.
