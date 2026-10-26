#!/usr/bin/env lua

local music_dir = (os.getenv("HOME") or "") .. "/music"
local links_file = music_dir .. "/links.txt"
local finished_file = music_dir .. "/finished.txt"
local failed_file = music_dir .. "/failed.txt"
local state_file = music_dir .. "/.current"

local pause_403 = tonumber(os.getenv("PAUSE_403")) or 1800
local retry_pause = tonumber(os.getenv("RETRY_PAUSE")) or 60
local max_fails = tonumber(os.getenv("MAX_FAILS")) or 5

local function log(msg)
  io.write(string.format("[%s] %s\n", os.date("%F %T"), msg))
  io.flush()
end

local function sh_quote(s)
  return "'" .. s:gsub("'", "'\\''") .. "'"
end

local function save_state(link, index)
  local f = io.open(state_file, "w")
  if f then
    f:write(link, "\t", tostring(index), "\n")
    f:close()
  end
end

local function read_state()
  local f = io.open(state_file, "r")
  if not f then return nil end
  local content = f:read("*l")
  f:close()
  if not content or content == "" then return nil end
  local link, index = content:match("^(.-)\t(%d+)$")
  if link and index then
    return link, tonumber(index)
  end
  return nil
end

local function append_file(path, line)
  local f = io.open(path, "a")
  if f then
    f:write(line, "\n")
    f:close()
  end
end

local function pick_link()
  local f = io.open(links_file, "r")
  if not f then return nil end
  local links = {}
  for line in f:lines() do
    local trimmed = line:match("^%s*(.-)%s*$")
    if trimmed and trimmed ~= "" then
      table.insert(links, trimmed)
    end
  end
  f:close()
  if #links == 0 then return nil end
  math.randomseed(os.time())
  return links[math.random(#links)]
end

local function remove_link(target)
  local f = io.open(links_file, "r")
  if not f then return end
  local lines = {}
  for line in f:lines() do
    local trimmed = line:match("^%s*(.-)%s*$")
    if trimmed ~= target then
      table.insert(lines, line)
    end
  end
  f:close()
  local tmp = music_dir .. "/.tmp.links." .. os.time()
  local out = io.open(tmp, "w")
  if out then
    for _, l in ipairs(lines) do
      out:write(l, "\n")
    end
    out:close()
    os.rename(tmp, links_file)
  end
end

local function run_download(link, start)
  local out_tmpl = music_dir .. "/%(uploader)s/%(title)s.%(ext)s"
  local extra = ""
  if start > 1 then
    extra = string.format("--playlist-start %d ", start)
  end
  local cmd = string.format(
    "echo $$; exec yt-dlp -x --audio-format mp3 --audio-quality 0 --embed-thumbnail --embed-metadata --newline -o %s %s%s 2>&1",
    sh_quote(out_tmpl),
    extra,
    sh_quote(link)
  )
  local p = io.popen(cmd, "r")
  if not p then return "error", start end
  local pid_str = p:read("*l")
  local pid = tonumber(pid_str)

  local index = start
  local result = "error"

  for line in p:lines() do
    io.write(line, "\n")
    io.flush()
    local item = line:match("%[download%] Downloading item (%d+) of %d+")
    if item then
      index = tonumber(item)
      save_state(link, index)
    elseif line:find("%[download%] Finished downloading playlist:") then
      result = "finished"
    elseif line:find("HTTP Error 403") then
      result = "403"
      if pid then
        os.execute("kill " .. pid .. " 2>/dev/null")
      end
      break
    end
  end

  local ok = p:close()
  if result == "error" and ok then
    result = "finished"
  end
  return result, index
end

local function process_link(link, start_index)
  local fails = 0
  local index = start_index

  while true do
    local prev_index = index
    log(string.format("Downloading: %s (starting at item %d)", link, index))
    local result, new_index = run_download(link, index)
    index = new_index

    if result == "finished" then
      log("Finished: " .. link)
      append_file(finished_file, link)
      remove_link(link)
      os.remove(state_file)
      return
    elseif result == "403" then
      log(string.format("HTTP 403 - freezing for %d min, then retrying item %d", math.floor(pause_403 / 60), index))
      os.execute("sleep " .. pause_403)
    else
      if index == prev_index then
        fails = fails + 1
      else
        fails = 0
      end
      if fails >= max_fails then
        log(string.format("Giving up on %s after %d failures -> %s", link, fails, failed_file))
        append_file(failed_file, link)
        remove_link(link)
        os.remove(state_file)
        return
      end
      log(string.format("yt-dlp stopped unexpectedly (failure %d/%d), retrying in %ds", fails, max_fails, retry_pause))
      os.execute("sleep " .. retry_pause)
    end
  end
end

os.execute("mkdir -p " .. sh_quote(music_dir))
local lf = io.open(links_file, "a"); if lf then lf:close() end
local ff = io.open(finished_file, "a"); if ff then ff:close() end

while true do
  local link, index = read_state()
  if link and index then
    log(string.format("Resuming unfinished link at item %d", index))
  else
    link = pick_link()
    if not link then
      log("links.txt is empty - nothing left to do")
      os.exit(0)
    end
    index = 1
    save_state(link, 1)
  end
  process_link(link, index)
end
