Stage = Object:extend()

function Stage:new()
    self.stats = Stats()

    self.grid = Grid()
    self:start()
end

function Stage:start()
    self:start_grid()
    self:start_stats()

    for i = 1, 2 do
        self:end_event_cell_new()
    end
end

function Stage:start_grid()
    for y = 1, GRID_SIZE do
        for x = 1, GRID_SIZE do
            local cell = self.grid.cells[y][x]
            self:start_cell(cell, {x, y})
        end
    end
end

function Stage:start_cell(cell, tile)
    cell.shape.x = (tile[1] - 1) * CELL_WIDTH
    cell.shape.y = (tile[2] - 1) * CELL_HEIGHT
    cell.value = EMPTY_CELL_VALUE
    cell.animation = false
end

function Stage:start_stats()
    self.stats.previous_pressed = false
end

function Stage:end_event_cell_new()
    local empty_cells = self.grid:get_empty_cells()
    if #empty_cells == 0 then
        return
    end

    local random_index = math.floor(random(1, #empty_cells))
    local empty_cell = empty_cells[random_index]
    local start_value = math.floor(math.pow(2, math.floor(random(1, 2.9))))

    self.grid.cells[empty_cell.y][empty_cell.x].value = start_value
    self.grid.cells[empty_cell.y][empty_cell.x].animating = false
end

function Stage:update(dt)
    self:handle_player_input()

    if self:is_animating() then
        self:update_animation(dt)
        if not self:is_animating() then
            self:update_animation_finished()
            self:end_event_cell_new()
            if self:is_win() then
                print('win')
            elseif self:is_lose() then
                print('lose')
                go_to_room('Stage')
            end
        end
    end
end

function Stage:draw()
    self.grid:draw_background()
    self.grid:draw_cells()
end

function Stage:is_animating()
    for y = 1, GRID_SIZE do
        for x = 1, GRID_SIZE do
            local cell = self.grid.cells[y][x]
            if cell.animating then
                return true
            end
        end
    end
    return false
end

function Stage:event_cells_action_rotate(turns)
    for t = 1, turns do
        local temp = {}
        for y = 1, GRID_SIZE do
            temp[y] = {}
            for x = 1, GRID_SIZE do
                temp[y][x] = {}
            end
        end

        for y = 1, GRID_SIZE do
            for x = 1, GRID_SIZE do
                temp[x][GRID_SIZE - y + 1] = self.grid.cells[y][x]
            end
        end

        self.grid.cells = temp
    end
end

function Stage:get_cell_left_destination(i, j)
    local k = j

    while k > 1 and self.grid.cells[i][k - 1].target_value == EMPTY_CELL_VALUE do
        k = k - 1
    end

    return k
end

function Stage:event_cells_action_move_left_prepare()
    for y = 1, GRID_SIZE do
        for x = 1, GRID_SIZE do
            local cell = self.grid.cells[y][x]
            cell.start_position = {x = cell.shape.x, y = cell.shape.y}
            cell.target_value = cell.value
        end
    end
end

function Stage:event_cells_action_move_left()
    self:event_cells_action_move_left_prepare()

    local merged = {}
    for y = 1, GRID_SIZE do
        merged[y] = {}
        for x = 1, GRID_SIZE do
            merged[y][x] = false
        end
    end

    for y = 1, GRID_SIZE do
        for x = 2, GRID_SIZE do
            if self.grid.cells[y][x].target_value ~= EMPTY_CELL_VALUE then
                self:event_cells_action_move_left_apply(merged, y, x)
            end
        end
    end
end

function Stage:event_cells_action_move_left_apply(merged, y, x)
    local k = self:get_cell_left_destination(y, x)
    if k > 1 and self.grid.cells[y][x].target_value == self.grid.cells[y][k - 1].target_value and not merged[y][k - 1] then
        merged[y][k - 1] = true

        self:event_cells_merge(self.grid.cells[y][x], self.grid.cells[y][k - 1])
        self.grid.cells[y][k - 1].target_value = self.grid.cells[y][k - 1].target_value * 2
        self.grid.cells[y][x].target_value = EMPTY_CELL_VALUE
    elseif self.grid.cells[y][k].target_value == EMPTY_CELL_VALUE then
        self:event_cells_merge(self.grid.cells[y][x], self.grid.cells[y][k])
        self.grid.cells[y][k].target_value = self.grid.cells[y][x].target_value
        self.grid.cells[y][x].target_value = EMPTY_CELL_VALUE
    end
end

function Stage:event_cells_merge(from_cell, to_cell)
    from_cell.end_position = to_cell.start_position
    from_cell.animating = true
    self.stats.animation_progress = 0
end

function Stage:handle_player_input()
    if self:is_animating() then

        return
    end

    local turns = -1

    if input:pressed('move_left') then
        turns = 0
    elseif input:pressed('move_down') then
        turns = 1
    elseif input:pressed('move_right') then
        turns = 2
    elseif input:pressed('move_up') then
        turns = 3
    end

    self:event_cell_action(turns)
end

function Stage:event_cell_action(turns)
    if turns > 0 then
        self:event_cells_action_rotate(turns)
        self:event_cells_action_move_left()
        self:event_cells_action_rotate(4 - turns)
    elseif turns == 0 then
        self:event_cells_action_move_left()
    end
end

function Stage:update_animation(dt)
    local progress = self.stats.animation_progress / ANIMATION_DURATION
    self.stats.animation_progress = self.stats.animation_progress + dt

    for y = 1, GRID_SIZE do
        for x = 1, GRID_SIZE do
            local cell = self.grid.cells[y][x]
            if cell.animating then
                self:update_animation_cell(cell, progress)
            end
        end
    end
end

function Stage:update_animation_cell(cell, progress)
    local x = cell.start_position.x + (cell.end_position.x - cell.start_position.x) * progress
    local y = cell.start_position.y + (cell.end_position.y - cell.start_position.y) * progress

    cell.shape.x = x
    cell.shape.y = y

    if self.stats.animation_progress >= ANIMATION_DURATION then
        cell.animating = false
    end
end

function Stage:update_animation_finished()
    for y = 1, GRID_SIZE do
        for x = 1, GRID_SIZE do
            local cell = self.grid.cells[y][x]
            self:update_animation_finished_cell(cell)
        end
    end
end

function Stage:update_animation_finished_cell(cell)
    cell.value = cell.target_value
    cell.shape.x = cell.start_position.x
    cell.shape.y = cell.start_position.y
end

function Stage:is_win()
    for y = 1, GRID_SIZE do
        for x = 1, GRID_SIZE do
            local cell = self.grid.cells[y][x]
            if cell.value == STATS_TARGET_VALUE then
                return true
            end
        end
    end

    return false
end

function Stage:is_lose()
    for y = 1, GRID_SIZE do
        for x = 1, GRID_SIZE do
            local value = self.grid.cells[y][x].value

            if value == EMPTY_CELL_VALUE then
                return false
            end

            if x < GRID_SIZE
                and value == self.grid.cells[y][x + 1].value
            then
                return false
            end

            if y < GRID_SIZE
                and value == self.grid.cells[y + 1][x].value
            then
                return false
            end
        end
    end

    return true
end
