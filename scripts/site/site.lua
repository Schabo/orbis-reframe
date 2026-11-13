-- Pandoc filter for the project site (scripts/build_site.sh): turns one
-- Markdown page of docs/manual/ or docs/formats/ into a site page.
--
-- Links and images are rewritten for the page's place on the site: another
-- page of the manual or the formats (foo.md#x) becomes its site address
-- (../foo/#x), the manual's images stay next to the manual, and anything
-- else in the repository (code, other docs) goes to GitHub at `site_ref`,
-- the version the page was taken from. The page's title and description
-- come from its first heading and first paragraph.
--
-- Metadata it reads (pandoc -M): site_section ("manual" | "formats"),
-- site_page (the file's name without .md), site_ref (a tag or "main"),
-- site_repo (the repository's GitHub URL).

local section, page, ref, repo

local function meta_string(meta, key)
  return meta[key] and pandoc.utils.stringify(meta[key]) or ""
end

-- `path` with "." and ".." segments resolved; nil if it climbs out of the
-- repository.
local function normalize(path)
  local out = {}
  for seg in path:gmatch("[^/]+") do
    if seg == ".." then
      if #out == 0 then return nil end
      table.remove(out)
    elseif seg ~= "." then
      table.insert(out, seg)
    end
  end
  return table.concat(out, "/")
end

-- The site's root, relative to this page: a section's README is the
-- section's own folder, every other page one folder further down.
local function root()
  return page == "README" and "../" or "../../"
end

local function rewrite(target)
  -- The manual or the formats on GitHub, written out in full: the site's.
  local repo_path = repo ~= "" and target:sub(1, #repo + 1) == repo .. "/"
    and target:sub(#repo + 2):match("^blob/[^/]+/(docs/%a+/[^/]+%.md#?.*)$")
  local path, frag, full
  if repo_path then
    path, frag = repo_path:match("^([^#]*)(#?.*)$")
    full = path
  elseif target == "" or target:match("^#") or target:match("^%a[%w+.-]*:") then
    return target -- this page, or another site (https:, mailto:)
  else
    path, frag = target:match("^([^#]*)(#?.*)$")
    full = normalize("docs/" .. section .. "/" .. path)
  end
  if not full then
    return target
  end
  local sec, name = full:match("^docs/(%a+)/([^/]+)%.md$")
  if sec == "manual" or sec == "formats" then
    local slug = name == "README" and "" or name .. "/"
    return root() .. sec .. "/" .. slug .. frag
  end
  if repo_path then
    return target -- another document on GitHub: as written
  end
  local image = full:match("^docs/manual/(images/.+)$")
  if image then
    return root() .. "manual/" .. image
  end
  -- A file has an extension; anything else is a folder.
  local last = full:match("[^/]*$")
  local kind = last:find(".", 1, true) and "blob" or "tree"
  if full == "" then
    return repo .. "/tree/" .. ref .. frag
  end
  return repo .. "/" .. kind .. "/" .. ref .. "/" .. full .. frag
end

-- src="..." / href="..." in raw HTML (the manual's icon images in tables).
local function rewrite_html(text)
  for _, attr in ipairs({ "src", "href" }) do
    text = text:gsub("(%f[%w]" .. attr .. '=")([^"]*)(")', function(pre, url, post)
      return pre .. rewrite(url) .. post
    end)
  end
  return text
end

function Link(el)
  el.target = rewrite(el.target)
  return el
end

function Image(el)
  el.src = rewrite(el.src)
  return el
end

function RawInline(el)
  if el.format == "html" then
    el.text = rewrite_html(el.text)
  end
  return el
end

function RawBlock(el)
  if el.format == "html" then
    el.text = rewrite_html(el.text)
  end
  return el
end

-- A search result's description: the first paragraph, cut at a word near
-- 155 characters.
local function description(text)
  text = text:gsub("%s+", " "):gsub("^ ", ""):gsub(" $", "")
  if #text <= 160 then
    return text
  end
  local cut = text:sub(1, 155):match("^(.*) ") or text:sub(1, 155)
  return cut:gsub("[,;:]$", "") .. "…"
end

local function first_pass(doc)
  section = meta_string(doc.meta, "site_section")
  page = meta_string(doc.meta, "site_page")
  ref = meta_string(doc.meta, "site_ref")
  repo = meta_string(doc.meta, "site_repo")
  local heading, para
  for _, b in ipairs(doc.blocks) do
    if not heading and b.t == "Header" and b.level == 1 then
      heading = pandoc.utils.stringify(b)
    elseif heading and not para and b.t == "Para" then
      para = pandoc.utils.stringify(b)
    end
  end
  heading = heading or page
  local title = heading
  if section == "manual" and page ~= "README" then
    title = heading .. " – orbis-reframe manual"
  elseif section == "formats" then
    title = heading .. " – orbis-reframe"
  end
  doc.meta.pagetitle = title
  doc.meta.heading = heading
  doc.meta.description = description(para or heading)
  return doc
end

return {
  { Pandoc = first_pass },
  { Link = Link, Image = Image, RawInline = RawInline, RawBlock = RawBlock },
}
