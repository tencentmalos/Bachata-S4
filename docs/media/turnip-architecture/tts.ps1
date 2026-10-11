# Synthesizes narration cues with the Windows SAPI voice (offline).
# Input: a UTF-8 manifest, one cue per line: <output.wav><TAB><text>.
# make_video.py writes the manifest and calls this script; it is ASCII-only so
# Windows PowerShell 5.1 parses it regardless of the console code page.
param(
    [Parameter(Mandatory = $true)][string]$Manifest,
    [string]$Voice = "Microsoft Huihui Desktop",
    [int]$Rate = 1,
    [int]$SampleRate = 24000
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Speech

$synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
$synth.SelectVoice($Voice)
$synth.Rate = $Rate
$format = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(
    $SampleRate,
    [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,
    [System.Speech.AudioFormat.AudioChannel]::Mono)

$lines = [IO.File]::ReadAllLines($Manifest, [Text.Encoding]::UTF8)
foreach ($line in $lines) {
    if ($line.Trim().Length -eq 0) { continue }
    $parts = $line.Split("`t", 2)
    $synth.SetOutputToWaveFile($parts[0], $format)
    $synth.Speak($parts[1])
    $synth.SetOutputToNull()
}
$synth.Dispose()
