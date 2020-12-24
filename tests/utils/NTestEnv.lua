-- A series of convenient utility functions for use with NTest on NodeMCU
-- devices under test (DUTs).

local NTE = {}

function NTE.hasC(m)
  -- this isn't great, but it's what we've got
  local mstr = node.info('build_config').modules
  return mstr:match("^"..m..",") -- first module in the list
      or mstr:match(","..m.."$") -- last module in the list
      or mstr:match(","..m..",") -- somewhere else
end

function NTE.hasL(m)
  for _, l in ipairs(package.loaders) do
    if type(l(m)) == "function" then return true end
  end
  return false
end

function NTE.getFeat(...)
  -- Stream through our configuration file and extract the features attested
  --
  -- We expect the configuration file to attest features as keys in a dictionary
  -- so that they can be efficiently probed here but also so that we can
  -- parameterize features.
  --
  -- { "features" : {
  --    "feat1" : ... ,
  --    "feat2" : ...
  --   }
  -- }
  local reqFeats = { ... }
  local decoder = sjson.decoder({
    metatable = {
      checkpath = function(_, p)
        if #p > 2 then return true end              -- something we're keeping
        if #p == 0 then return true end             -- root table
        if p[1] == "features" then
          if #p == 1 then return true end           -- features table
          local thisFeat = p[2]
          assert (type(thisFeat) == "string")
          for _, v in ipairs(reqFeats) do
            if v == thisFeat then return true end   -- requested feature
          end
        end
        return false
      end
    }
  })
  local cfgf = file.open("testenv.conf", "r")
  assert (cfgf ~= nil, "Missing testenv.conf")
  local cstr
  repeat cstr = cfgf:read(); decoder:write(cstr) until #cstr == 0
  cfgf:close()
  local givenFeats = decoder:result().features
  assert (type(givenFeats) == "table", "Malformed configuration file")

  local res = {}
  for _, v in ipairs(reqFeats) do
    res[v] = givenFeats[v] or error("Missing required feature " .. v)
  end

  return res
end

return NTE
