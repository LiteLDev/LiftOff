# The game keeps server resource packs in its pack cache by uuid + version. Development builds all
# carry version 0.0.1, so a client that joined an older build keeps the old animations: drop our
# cached copy so remote modes download the current LiftOff RP.
$cache = Join-Path (Get-GameDataDir) 'minecraftpe\packcache'
if (Test-Path $cache) {
    Get-ChildItem $cache -Recurse -Filter manifest.json -ErrorAction SilentlyContinue |
        Where-Object { (Get-Content $_.FullName -Raw) -match '0e33f390-6bed-4ef0-b0ed-fd4b1fb5de77' } |
        ForEach-Object {
            Remove-Item -Recurse -Force $_.DirectoryName
            Write-Host "[e2e] removed cached LiftOff RP $($_.DirectoryName)"
        }
}
