Sudoku = {}
math.randomseed(os.time())

function Sudoku.printHeadline()
  io.write(" ░██████╗██╗░░░██╗██████╗░░█████╗░██╗░░██╗██╗░░░██╗\n")
  io.write(" ██╔════╝██║░░░██║██╔══██╗██╔══██╗██║░██╔╝██║░░░██║\n")
  io.write(" ╚█████╗░██║░░░██║██║░░██║██║░░██║█████═╝░██║░░░██║\n")
  io.write(" ░╚═══██╗██║░░░██║██║░░██║██║░░██║██╔═██╗░██║░░░██║\n")
  io.write(" ██████╔╝╚██████╔╝██████╔╝╚█████╔╝██║░╚██╗╚██████╔╝\n")
  io.write(" ╚═════╝░░╚═════╝░╚═════╝░░╚════╝░╚═╝░░╚═╝░╚═════╝░\n")
end

function Sudoku.printBoard(tbl)
  io.write("╔═══╤═══╤═══╦═══╤═══╤═══╦═══╤═══╤═══╗")
  io.write("\n")

  for i = 1, #tbl do
    local row = tbl[i]
    io.write("║")
    for j = 1, #row do
      local value
      if row[j] == 0 then
        value = " "
      else
        value = row[j]
      end
      io.write(" " .. value .. " ")
      if j == 3 or j == 6 or j == 9 then
        io.write("║")
      else
        io.write("│")
      end
    end

    if i == 3 or i == 6 then
      io.write("\n")
      io.write("╠═══╪═══╪═══╬═══╪═══╪═══╬═══╪═══╪═══╣")
    elseif i ~= 9 then
      io.write("\n")
      io.write("╟───┼───┼───╫───┼───┼───╫───┼───┼───╢")
    end
    if i < #tbl then
      io.write("\n")
    end
  end
  io.write("\n")
  io.write("╚═══╧═══╧═══╩═══╧═══╧═══╩═══╧═══╧═══╝")
  io.write("\n")
end

function Sudoku.createEmptyBoard()
  local board = {}
  for i = 1, 9 do
    board[i] = {}
    for j = 1, 9 do
      board[i][j] = 0
    end
  end
  return board
end

function Sudoku.convertBoardToCode(board)
  local code = ""
  for _, row in ipairs(board) do
    for _, value in ipairs(row) do
      code = code .. tostring(value)
    end
  end

  return code
end

function Sudoku.findNextEmptyCell(board)
  for row = 1, #board do
    for col = 1, #board[row] do
      if board[row][col] == 0 then
        return row, col
      end
    end
  end
end

function Sudoku.isValidPlacement(board, num, row, col)
  if board[row][col] ~= 0 then
    return false
  end

  -- Check if the number is already in the row
  for i = 1, #board[row] do
    if board[row][i] == num then
      return false
    end
  end

  -- Check if the number is already in the column
  for i = 1, #board do
    if board[i][col] == num then
      return false
    end
  end

  -- Calculate the top-left corner of the 3x3 internal box
  local boxRowStart = row - ((row - 1) % 3)
  local boxColStart = col - ((col - 1) % 3)

  -- Check if the number is already in the internal 3x3 box
  for i = 0, 2 do
    for j = 0, 2 do
      if board[boxRowStart + i][boxColStart + j] == num then
        return false
      end
    end
  end

  return true
end

-- This function basically solves the sudoku
function Sudoku.solveBoard(board)
  local row, col = Sudoku.findNextEmptyCell(board)

  if not row then
    return true -- puzzle is solved
  end

  for num = 1, 9 do
    if Sudoku.isValidPlacement(board, num, row, col) then
      board[row][col] = num
      if Sudoku.solveBoard(board) then
        return true       -- the puzzle is solved
      end
      board[row][col] = 0 -- reset the cell and try the next number
    end
  end


  return false -- Trigger backtracking
end

-- now using a method to generate a playable sudoku board pre-filled
-- with some of the options for a player to play on
function math.randomchoice(tablelist)
  local index = math.random(#tablelist)
  return table.remove(tablelist, index)
end

function Sudoku.fillBoardWithSolution(board)
  for row = 1, 9 do
    for col = 1, 9 do
      if board[row][col] == 0 then
        for num = 1, 9 do
          if Sudoku.isValidPlacement(board, num, row, col) then
            board[row][col] = num
            if Sudoku.solveBoard(board) then
              Sudoku.fillBoardWithSolution(board)
              return board
            end

            board[row][col] = 0
          end
        end

        return false
      end
    end
  end
end

function Sudoku.generateFullSolutionBoard(board)
  local list = { 1, 2, 3, 4, 5, 6, 7, 8, 9 }
  -- populate the first 3x3
  for row = 1, 3 do
    for col = 1, 3 do
      local num = math.randomchoice(list)
      board[row][col] = num
    end
  end

  -- populate the second 3x3
  list = { 1, 2, 3, 4, 5, 6, 7, 8, 9 }
  for row = 4, 6 do
    for col = 4, 6 do
      local num = math.randomchoice(list)
      board[row][col] = num
    end
  end

  -- populate the last 3x3
  list = { 1, 2, 3, 4, 5, 6, 7, 8, 9 }
  for row = 7, 9 do
    for col = 7, 9 do
      local num = math.randomchoice(list)

      board[row][col] = num
    end
  end

  return Sudoku.fillBoardWithSolution(board)
end

-- Now the code about handling the solution and finding the possible solutions

function Sudoku.atemptSolveFromCell(board, row, col)
  for num = 1, 9 do
    if Sudoku.isValidPlacement(board, num, row, col) then
      board[row][col] = num

      if Sudoku.solve(board) then
        return true
      end

      board[row][col] = 0 -- Reset the cell if the placement doesn't lead to a solution
    end
  end

  return false
end

function Sudoku.findNthEmptyCell(board, h)
  local k = 1
  for row = 1, #board do
    for col = 1, #board[row] do
      if board[row][col] == 0 then
        if k == h then
          return row, col
        end
        k = k + 1
      end
    end
  end

  return false
end

local function deepcopy(orig)
  local orig_type = type(orig)
  local copy
  if orig_type == 'table' then
    copy = {}
    for orig_key, orig_value in next, orig, nil do
      copy[deepcopy(orig_key)] = deepcopy(orig_value)
    end
    setmetatable(copy, deepcopy(getmetatable(orig)))
  else -- number, string, boolean, etc
    copy = orig
  end
  return copy
end

function Sudoku.coundUniqueSolutions(board)
  local z = 0
  local list_of_solutions = {}

  -- Count empty empty spaces
  for row = 1, #board do
    for col = 1, #board[row] do
      if board[row][col] == 0 then
        z = z + 1
      end
    end
  end

  -- Attempt to find solutions from each empty space
  for i = 1, z do
    local board_copy = deepcopy(board)
    local row, col = Sudoku.findNthEmptyCell(board_copy, i)
    if row and col then
      if Sudoku.atemptSolveFromCell(board_copy, row, col) then
        local solution_code = Sudoku.convertBoardToCode(board_copy)
        list_of_solutions[solution_code] = true
      end
    end
  end

  -- Extract unique solutions
  local unique_solutions = {}
  for solution_code, _ in pairs(list_of_solutions) do
    table.insert(unique_solutions, solution_code)
  end

  return unique_solutions
end

function Sudoku.createPuzzle(fullBoard, difficulty)
  local board = deepcopy(fullBoard)
  local squares_to_remove

  if difficulty == 0 then
    squares_to_remove = 36
  elseif difficulty == 1 then
    squares_to_remove = 46
  elseif difficulty == 2 then
    squares_to_remove = 52
  else
    squares_to_remove = difficulty
  end

  local function removeCellsRandomly(numToRemove)
    local counter = 0
    while counter < numToRemove do
      local row = math.random(1, 9)
      local col = math.random(1, 9)
      if board[row][col] ~= 0 then
        board[row][col] = 0
        counter = counter + 1
      end
    end
  end

  -- initial removal from specific boxes to ensure minimal solvability
  removeCellsRandomly(4)

  -- additional removals based on difficulty
  squares_to_remove = squares_to_remove - 12
  removeCellsRandomly(squares_to_remove)

  return board, fullBoard
end

function Sudoku.initializeGame()
  local board = Sudoku.generateFullSolutionBoard(Sudoku.createEmptyBoard())
  local playableBoard, fullBoard = Sudoku.createPuzzle(board, 0)

  return playableBoard, fullBoard
end

return Sudoku
