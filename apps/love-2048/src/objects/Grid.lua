Grid = Object:extend()

function Grid:new()
    self:init_background()
    self:init_cells()
end

function Grid:init_background()
    self.background = {}
    for y = 1, GRID_SIZE do
        self.background[y] = {}
        for x = 1, GRID_SIZE do
            self.background[y][x] = {x = (x - 1)*CELL_WIDTH, y = (y - 1)*CELL_HEIGHT, w = CELL_WIDTH, h = CELL_HEIGHT}
        end
    end
end

function Grid:init_cells()
    self.cells = {}
    for y = 1, GRID_SIZE do
        self.cells[y] = {}
        for x = 1, GRID_SIZE do
            self.cells[y][x] = Cell()
            self.cells[y][x].value = y..x
        end
    end
end

function Grid:draw_background()
    for y = 1, GRID_SIZE do
        for x = 1, GRID_SIZE do
            local background_item = self.background[y][x]
            love.graphics.setColor(CELL_COLOR_EMPTY)
            love.graphics.rectangle('fill', background_item.x, background_item.y, background_item.w, background_item.h)
            love.graphics.setColor(0, 0, 0)
            love.graphics.rectangle('line', background_item.x, background_item.y, background_item.w, background_item.h)
        end
    end
    love.graphics.setColor(1,1,1)
end

function Grid:draw_cells()
    for y = 1, GRID_SIZE do
        for x = 1, GRID_SIZE do
            local cell = self.cells[y][x]
            if cell.value > EMPTY_CELL_VALUE then
                cell:draw() 
            end
        end
    end
end

function Grid:get_empty_cells()
    local empty_cells = {}
    local index = 1
    for y = 1, GRID_SIZE do
        for x = 1, GRID_SIZE do
           if self.cells[y][x].value == EMPTY_CELL_VALUE then
                table.insert(empty_cells, index, {y = y, x = x})
                index = index + 1
           end 
        end
    end

    return empty_cells
end
--[[
    std::vector<sf::Vector2i> Game::GetEmptyCells() const
{
    std::vector<sf::Vector2i> emptyCells;

    for (int i = 0; i < GRID_SIZE; i++)
    {
        for (int j = 0; j < GRID_SIZE; j++)
        {
            if (grid.cells[i][j].value == 0)
            {
                emptyCells.emplace_back(i, j);
            }
        }
    }

    return emptyCells;
}

]]