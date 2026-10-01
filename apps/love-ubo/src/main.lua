require 'game'

function love.load()
	--love.mouse.setVisible(false)
	Game:load(12)
	game = Game()
end

function love.draw() 
	game:draw()
end


function love.update(dt)
	game:update(dt)
end

function love.mousepressed(x,y,b)
	game:mousepressed(x,y,b)
end

function love.keypressed(key)
	game:keypressed(key)
end
