STATS_TARGET_VALUE = 2048
GRID_SIZE = 4
CELL_HEIGHT = SCREEN_HEIGHT / GRID_SIZE
CELL_WIDTH = SCREEN_WIDTH / GRID_SIZE
EMPTY_CELL_VALUE = 1
ANIMATION_DURATION = 0.09
CELL_TEXT_SIZE = 80
CELL_COLOR_EMPTY = {120/255, 110/255, 100/255}
CELLS_COLORS = {
    [0]    = {20 / 255, 20 / 255, 20 / 255},
    [1]    = {138 / 255, 128 / 255, 118 / 255},
    [2]    = {137 / 255, 124 / 255, 100 / 255},
    [3]    = {142 / 255, 77 / 255, 21 / 255},
    [4]   = {145 / 255, 49  / 255, 0},
    [5]   = {146 / 255, 24 / 255, 0},
    [6]   = {146 / 255, 0, 0},
    [7]  = {137 / 255, 107 / 255, 0},
    [8]  = {137 / 255, 104 / 255, 0},
    [9]  = {137 / 255, 100 / 255, 0},
    [10] = {137 / 255, 97 / 255, 0},
    [11] = {137 / 255, 94 / 255, 0}
}

CELL_TEXT_COLORS = {
    {139 / 255, 136 / 255, 132 / 255},
    {69 / 255, 59 / 255, 51 / 255} 
}

DEFAULT_FONT = love.graphics.newFont('assets/fonts/Super Meatball.ttf', CELL_TEXT_SIZE)