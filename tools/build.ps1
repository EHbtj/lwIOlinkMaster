param(
    [ValidateSet('c3','s3','all')][string]$Board='all',
    [string]$ArduinoCli='arduino-cli',
    [string]$ArduinoData="$env:LOCALAPPDATA/Arduino15",
    [Parameter(Mandatory=$true)][string]$ArduinoJson
)
$ErrorActionPreference='Stop'
$repoRoot=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$dataRoot=(Resolve-Path -LiteralPath $ArduinoData).Path.Replace('\','/')
$jsonRoot=(Resolve-Path -LiteralPath $ArduinoJson).Path
$properties=Get-Content -LiteralPath (Join-Path $jsonRoot 'library.properties')
if ('version=7.4.3' -notin $properties) { throw 'ArduinoJson 7.4.3 is required' }
if (!(Test-Path -LiteralPath "$dataRoot/packages/esp32/hardware/esp32/3.3.11")) {
    throw 'Install esp32:esp32@3.3.11 before building'
}
$buildRoot=Join-Path $repoRoot 'build'
New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null
$configPath=Join-Path $buildRoot 'arduino-cli.yaml'
$config=@{
    directories=@{data=$dataRoot;downloads=(Join-Path $buildRoot 'downloads')}
} | ConvertTo-Json -Depth 3
# JSON is accepted as a YAML subset. Keep machine-specific paths in ignored build/.
[IO.File]::WriteAllText($configPath,$config)
$platformList=& $ArduinoCli core list --config-file $configPath --format json
if($LASTEXITCODE -ne 0) { throw 'Could not inspect installed Arduino cores' }
$platform=(($platformList -join "`n") | ConvertFrom-Json).platforms | Where-Object id -eq 'esp32:esp32'
if($platform.installed_version -ne '3.3.11') { throw 'The active Arduino-ESP32 core must be 3.3.11' }
$targets=@{
    c3='esp32:esp32:XIAO_ESP32C3:CDCOnBoot=default'
    s3='esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc'
}
$chosen=if($Board -eq 'all') {@('c3','s3')} else {@($Board)}
foreach($target in $chosen) {
    & $ArduinoCli compile --config-file $configPath --fqbn $targets[$target] `
        --library $repoRoot --library $jsonRoot --build-path (Join-Path $buildRoot $target) `
        (Join-Path $repoRoot 'examples/USB_IOLinkMaster')
    if($LASTEXITCODE -ne 0) { throw "Arduino build failed: $target" }
}
