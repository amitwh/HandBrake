# HandBrake [![macOS Build](https://github.com/HandBrake/HandBrake/workflows/macOS%20build/badge.svg)](https://github.com/HandBrake/HandBrake/actions?query=workflow%3A%22macOS+build%22) [![Windows Build](https://github.com/HandBrake/HandBrake/workflows/Windows%20Build/badge.svg)](https://github.com/HandBrake/HandBrake/actions?query=workflow%3A%22Windows+Build%22) [![Linux Build](https://github.com/HandBrake/HandBrake/workflows/Linux%20Build/badge.svg)](https://github.com/HandBrake/HandBrake/actions?query=workflow%3A%22Linux+Build%22)


> ## Fork additions (1.11.2-1~fork3)
>
> This fork extends HandBrake 1.11.2 with:
>
> - **Smart join** - combine multiple files into one title; lossless
>   stream copy when all inputs match (video copied byte-for-byte),
>   frame-accurate re-encode otherwise, with a logged eligibility matrix
>   (codec, SPS/PPS, geometry, frame rate, colour, audio).
> - **Per-input trimming** - `--range` windows on each input;
>   keyframe-snapped in copy mode, frame-accurate when re-encoding.
> - **Edit filters** - playback speed (0.25x-4x, pitch-preserving),
>   PNG/JPEG watermark, and FreeType timestamp burn-in - in the CLI and
>   the GTK4 GUI (Filters tab).
> - **Pure-Mesa VAAPI** encode/decode for AMD APUs (no AMDGPU-PRO
>   needed): `vaapi_h264`/`vaapi_h265`/`vaapi_h265_10bit` encoders and
>   `--enable-hw-decoding vaapi`.
> - **Native Debian packaging** (`pkg/linux/debian`).
>
> See **[docs/JOIN-GUIDE.md](docs/JOIN-GUIDE.md)** for usage (GUI +
> CLI), and the workstream plans under `docs/plans/` for design and
> implementation notes. Build debs with:
> `mkdir build && cd build && ../configure --prefix=/usr --enable-vce --enable-vaapi --force && make -j$(nproc) pkg.create.deb`.

HandBrake is an open-source video transcoder available for Linux, Mac, and Windows, licensed under the [GNU General Public License (GPL) Version 2](LICENSE).

HandBrake takes videos you already have and makes new ones that work on your mobile phone, tablet, TV media player, game console, computer, or web browser—nearly anything that supports modern video formats.

HandBrake works with most common video files and formats, including ones created by consumer and professional video cameras, mobile devices such as phones and tablets, game and computer screen recordings, and DVD and Blu-ray discs. HandBrake leverages tools such as FFmpeg, x264, and x265, SVT-AV1 to create new MP4, MKV or WebM video files from these *Sources*.

For information on downloading, building/installing, and using HandBrake, see the official [HandBrake Documentation](https://handbrake.fr/docs).

## Community Support

For information on HandBrake's community support channels, please see [Community Support](https://handbrake.fr/docs/en/latest/help/community-support.html).

Please familiarise yourself with our [code of conduct](https://github.com/HandBrake/HandBrake/blob/master/CODE_OF_CONDUCT.md).

## Contributing

We welcome most contributions. While it is our goal to allow everyone to contribute, contributions not meeting the project's goals or standards may be rejected.

Please read our [guide to contributing](https://handbrake.fr/docs/en/latest/contributing/contribute.html). This will provide you with all the information you need to start contributing to the project. 

## Translations

We are now accepting translations via  [Transifex](https://explore.transifex.com/HandBrakeProject/) 

Please read our [Translations Guide](https://github.com/HandBrake/HandBrake/blob/master/TRANSLATION.markdown) and follow the instructions if you are interested in joining the translation effort.


## Additional Information

[Application / Project Information](https://github.com/HandBrake/HandBrake/wiki/Application-Information)  
[Authors](AUTHORS.markdown)  
[License](LICENSE)  
[News](NEWS.markdown)  

## Special Thanks

<a href="https://www.macstadium.com/"><img width="200" alt="MacStadium" src="https://uploads-ssl.webflow.com/5ac3c046c82724970fc60918/5c019d917bba312af7553b49_MacStadium-developerlogo.png"></a>

and to many others who have contributed! [Thanks](THANKS.markdown)
