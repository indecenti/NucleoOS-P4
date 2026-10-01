-- colors is a module providing color palette constants based on the vice c64 palette

local colors = {}

-- palette: https://lospec.com/palette-list/ty-black-souls-16
local palette = {
    BLACK_SOULS_1  = {223,205,183},
    BLACK_SOULS_2  = {197,164,123},
    BLACK_SOULS_3  = {154,125, 74},
    BLACK_SOULS_4  = {152,159, 63},
    BLACK_SOULS_5  = {99, 101, 74},
    BLACK_SOULS_6  = {41,  64, 52},
    BLACK_SOULS_7  = {129,165,152},
    BLACK_SOULS_8  = {69, 107,117},
    BLACK_SOULS_9  = {49,  48, 69},
    BLACK_SOULS_10 = {197,129, 50},
    BLACK_SOULS_11 = {133, 75, 47},
    BLACK_SOULS_12 = {80,  35, 23},
    BLACK_SOULS_13 = {159, 87, 69},
    BLACK_SOULS_14 = {96,  65, 69},
    BLACK_SOULS_15 = {53,  46, 50},
    BLACK_SOULS_16 = {37,  29, 34}
}

function colors.init()
	for label,color in pairs(palette) do
		for i = 1,3 do
			palette[label][i] = palette[label][i] / 255
		end
	end
end

function colors.get(color)
	return palette[color]
end

return colors