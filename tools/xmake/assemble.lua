-- Assembles bin/<target_type>/LiftOff/ after the DLL is linked:
--   LiftOff.dll/.pdb, manifest.json, lang/<locale>.json,
--   behavior_packs/LiftOff_BP/, resource_packs/LiftOff_RP/
--
-- Fragment merging (each feature unit owns its own fragment files; duplicates fail the build):
--   assets/lang/<locale>/*.json               -> lang/<locale>.json
--   assets/pack/rp/item_texture.d/*.json      -> RP textures/item_texture.json   (texture_data entries)
--   assets/pack/rp/terrain_texture.d/*.json   -> RP textures/terrain_texture.json
--   assets/pack/rp/texts.d/<locale>/*.lang    -> RP texts/<locale>.lang (+ texts/languages.json)
--   assets/pack/rp/sounds.d/*.json            -> RP sounds/sound_definitions.json (sound_definitions entries)
--   assets/pack/rp/blocks.d/*.json            -> RP blocks.json (per-block "sound" / "textures" entries)
-- Empty merges write nothing (an empty Lua table would be saved as a JSON array).
-- Everything else under assets/pack/{bp,rp} is copied as is (directories ending in ".d" are skipped).
-- "${version}" in pack manifests is replaced by the mod version as a JSON array [major, minor, patch].

import("core.base.json")

local function mod_version()
    local tag = try { function () return os.iorun("git describe --tags --abbrev=0 --always") end }
    local major, minor, patch = (tag or ""):match("v(%d+)%.(%d+)%.(%d+)")
    return tonumber(major) or 0, tonumber(minor) or 0, tonumber(patch) or 0
end

local function merge_json_fragments(dir, what)
    local merged = {}
    local origin = {}
    for _, file in ipairs(os.files(path.join(dir, "*.json"))) do
        for k, v in pairs(json.loadfile(file)) do
            if merged[k] ~= nil then
                raise("duplicate %s key '%s' in %s (already defined in %s)", what, k, file, origin[k])
            end
            merged[k] = v
            origin[k] = file
        end
    end
    return merged
end

local function merge_lang(projectdir, outdir)
    os.mkdir(outdir)
    for _, localedir in ipairs(os.dirs(path.join(projectdir, "assets", "lang", "*"))) do
        local merged = merge_json_fragments(localedir, "lang")
        if table.empty(merged) then merged = {["lo.lang.empty"] = ""} end
        json.savefile(path.join(outdir, path.filename(localedir) .. ".json"), merged)
    end
end

local function is_fragment_path(rel)
    for part in rel:gmatch("[^/\\]+") do
        if part:match("%.d$") then return true end
    end
    return false
end

local function copy_pack(src, dst, version)
    os.tryrm(dst)
    os.mkdir(dst)
    for _, file in ipairs(os.files(path.join(src, "**"))) do
        local rel = path.relative(file, src)
        if not is_fragment_path(rel) then
            local target = path.join(dst, rel)
            os.mkdir(path.directory(target))
            if path.filename(file) == "manifest.json" then
                local text = io.readfile(file)
                text = text:gsub('"%${version}"', string.format("[%d, %d, %d]", version[1], version[2], version[3]))
                io.writefile(target, text)
            else
                os.cp(file, target)
            end
        end
    end
end

local function merge_atlas(src, dst, fragdir, filename, atlas)
    local dir = path.join(src, fragdir)
    if not os.isdir(dir) then return end
    local data = merge_json_fragments(dir, filename)
    if table.empty(data) then return end
    local out = path.join(dst, "textures", filename)
    os.mkdir(path.directory(out))
    json.savefile(out, {resource_pack_name = "liftoff", texture_name = atlas, texture_data = data})
end

local function merge_sounds(src, dst)
    local dir = path.join(src, "sounds.d")
    if not os.isdir(dir) then return end
    local data = merge_json_fragments(dir, "sound_definitions.json")
    if table.empty(data) then return end
    local out = path.join(dst, "sounds", "sound_definitions.json")
    os.mkdir(path.directory(out))
    json.savefile(out, {format_version = "1.20.20", sound_definitions = data})
end

local function merge_blocks(src, dst)
    local dir = path.join(src, "blocks.d")
    if not os.isdir(dir) then return end
    local data = merge_json_fragments(dir, "blocks.json")
    if table.empty(data) then return end
    data["format_version"] = "1.21.40"
    json.savefile(path.join(dst, "blocks.json"), data)
end

local function merge_texts(src, dst)
    local textsd = path.join(src, "texts.d")
    if not os.isdir(textsd) then return end
    local locales = {}
    for _, localedir in ipairs(os.dirs(path.join(textsd, "*"))) do
        local locale = path.filename(localedir)
        local seen = {}
        local lines = {}
        for _, file in ipairs(os.files(path.join(localedir, "*.lang"))) do
            for line in io.lines(file) do
                line = line:gsub("\r$", "")
                local key = line:match("^([^#=][^=]*)=")
                if key then
                    if seen[key] then
                        raise("duplicate texts key '%s' in %s (already defined in %s)", key, file, seen[key])
                    end
                    seen[key] = file
                    table.insert(lines, line)
                end
            end
        end
        local out = path.join(dst, "texts", locale .. ".lang")
        os.mkdir(path.directory(out))
        io.writefile(out, table.concat(lines, "\n") .. "\n")
        table.insert(locales, locale)
    end
    table.sort(locales)
    json.savefile(path.join(dst, "texts", "languages.json"), locales)
end

function main(target, target_type)
    local projectdir = os.projectdir()
    local outdir = path.join(projectdir, "bin", target_type, "LiftOff")
    os.mkdir(outdir)

    -- Binaries and manifest (same substitutions as levibuildscript's modpacker).
    local targetfile = target:targetfile()
    os.cp(targetfile, path.join(outdir, path.filename(targetfile)))
    local pdb = path.join(path.directory(targetfile), path.basename(targetfile) .. ".pdb")
    if os.isfile(pdb) then os.cp(pdb, path.join(outdir, path.basename(targetfile) .. ".pdb")) end
    local major, minor, patch = mod_version()
    local manifest = io.readfile(path.join(projectdir, "manifest.json"))
    local vars = {
        modName = "LiftOff",
        modFile = path.filename(targetfile),
        modVersion = string.format("%d.%d.%d", major, minor, patch),
        modPlatform = target_type,
    }
    manifest = manifest:gsub("%${(.-)}", function (k) return vars[k] or ("${" .. k .. "}") end)
    io.writefile(path.join(outdir, "manifest.json"), manifest)

    merge_lang(projectdir, path.join(outdir, "lang"))

    local version = {major, minor, patch}
    if major == 0 and minor == 0 and patch == 0 then version = {0, 0, 1} end -- packs reject 0.0.0
    local bpsrc = path.join(projectdir, "assets", "pack", "bp")
    local rpsrc = path.join(projectdir, "assets", "pack", "rp")
    local bpdst = path.join(outdir, "behavior_packs", "LiftOff_BP")
    local rpdst = path.join(outdir, "resource_packs", "LiftOff_RP")
    copy_pack(bpsrc, bpdst, version)
    copy_pack(rpsrc, rpdst, version)
    merge_atlas(rpsrc, rpdst, "item_texture.d", "item_texture.json", "atlas.items")
    merge_atlas(rpsrc, rpdst, "terrain_texture.d", "terrain_texture.json", "atlas.terrain")
    merge_sounds(rpsrc, rpdst)
    merge_blocks(rpsrc, rpdst)
    merge_texts(rpsrc, rpdst)

    cprint("${bright green}[assemble]: ${reset}%s", outdir)
end
