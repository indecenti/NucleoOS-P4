GAME_MODES = {
    ["Beginner"] = {
        -- Amount of mines
        mines = 10,
        -- Amount of flags available
        flags = 10,

        -- Board starting x coordinate relative to window coordinates
        boardX = 0,
        -- Board starting y coordinate relative to window coordinates
        boardY = 200,
        -- Board width (x-axis) NOT in pixels, but in blocks
        boardWidth = 10,
        -- Board height (y-axis) NOT in pixels, but in blocks
        boardHeight = 10,
        -- Window width
        windowWidth = 800,
        -- Window height
        windowHeight = 1000,

        -- Timer text x coordinate
        timerX = 380,
        -- Timer text y coordinate
        timerY = 50,

        -- Flags available text x coordinate
        flagX = 700,
        -- Flags available text y coordinate
        flagY = 50,

        -- Block scale factor
        blockScale = 5

    },

    ["Medium"] = {
        -- Amount of mines
        mines = 40,
        -- Amount of flags available
        flags = 40,

        -- Board starting x coordinate relative to window coordinates
        boardX = 0,
        -- Board starting y coordinate relative to window coordinates
        boardY = 200,
        -- Board width (x-axis) NOT in pixels, but in blocks
        boardWidth = 16,
        -- Board height (y-axis) NOT in pixels, but in blocks
        boardHeight = 16,
        -- Window width
        windowWidth = 1024,
        -- Window height
        windowHeight = 1224,

        -- Timer text x coordinate
        timerX = 500,
        -- Timer text y coordinate
        timerY = 50,

        -- Flags available text x coordinate
        flagX = 930,
        -- Flags available text y coordinate
        flagY = 50,

        -- Block scale factor
        blockScale = 4

    },

    ["Hard"] = {
        -- Amount of mines
        mines = 99,
        -- Amount of flags available
        flags = 99,

        -- Board starting x coordinate relative to window coordinates
        boardX = 0,
        -- Board starting y coordinate relative to window coordinates
        boardY = 200,
        -- Board width (x-axis) NOT in pixels, but in blocks
        boardWidth = 30,
        -- Board height (y-axis) NOT in pixels, but in blocks
        boardHeight = 16,
        -- Window width
        windowWidth = 1440,
        -- Window height
        windowHeight = 968,

        -- Timer text x coordinate
        timerX = 700,
        -- Timer text y coordinate
        timerY = 50,

        -- Flags available text x coordinate
        flagX = 1340,
        -- Flags available text y coordinate
        flagY = 50,

        -- Block scale factor
        blockScale = 3

    }

}

-- Difficulty
config = GAME_MODES["Beginner"]

function love.conf(t)
    t.window.title = "Minesweeper"
    t.window.width = config.windowWidth
    t.window.height = config.windowHeight
end