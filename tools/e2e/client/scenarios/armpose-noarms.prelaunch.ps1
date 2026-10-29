# renderArms = false in the client's carryon_client.json (the deployed mod is a fresh copy). The file
# must be complete: missing fields fail the load.
. (Join-Path $PSScriptRoot 'armpose.prelaunch.ps1')
$cfg = Join-Path (Join-Path $script:ModsDir $script:ModName) 'config'
New-Item -ItemType Directory -Force -Path $cfg | Out-Null
@{
    version        = 1
    settings       = [ordered]@{ facePlayer = $false; rotateEntitiesSideways = $false; renderArms = $false }
    modelOverrides = @{
        modelOverrides = @(
            'minecraft:redstone_wire->(item)minecraft:redstone',
            'minecraft:bamboo_sapling->(block)minecraft:bamboo',
            'minecraft:candle_cake->(block)minecraft:cake'
        )
    }
} | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8NoBOM (Join-Path $cfg 'carryon_client.json')
Write-Host "[e2e] renderArms = false in $cfg"
