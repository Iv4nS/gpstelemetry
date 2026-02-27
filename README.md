## Description

gpstelemetry is a quick and dirty command-line GPS telemetry extractor for GoPro.

Its text output emulates the output format of the web-based ["Telemetry Extractor Lite"](https://goprotelemetryextractor.com/free/), but unlike that tool, it doesn't require providing some company with your personal information and storing browser cookies to authenticate your registration.  Also, running code locally instead of in a browser is generally much faster.

The code leverages GoPro's [gpmf-parser](https://github.com/gopro/gpmf-parser) library.

## Building

After initially downloading this project's code, issue the following command to download the gpmf-parser library submodule:

```
git submodule update --init --recursive
```

Then "make" the code.

## Usage

```
gpstelemetry [options] <mp4file> [mp4file_2] ... [mp4file_n]
```

### Options

| Option | Description |
|--------|-------------|
| `--print_filename` | Include the filename in output |
| `--print_filepath` | Include the full file path in output |
| `--min_fix=N` | Only output entries with fix >= N |
| `--max_precision=N` | Only output entries with precision <= N |

## Examples

```
gpstelemetry myfile.mp4
```

or if you want the output to go to a file instead of stdout:

```
gpstelemetry myfile.mp4 > mydata.csv
```

GoPro splits recordings into many files, even though these represent a single recording.  The tool can produce a single output file when all the constituent input files are provided, in order, as command line arguments:

```
gpstelemetry GL010009.LRV GL020009.LRV GL030009.LRV GL040009.LRV GL050009.LRV > myjourney.csv
```

Filter to only include entries with good GPS fix and precision:

```
gpstelemetry --min_fix=3 --max_precision=100 --print_filename myfile.mp4
```

