require 'mobile'

local lg=love.graphics

Ubo = Mobile:subclass('Ubo')

function Ubo:initialize(grid,x,y)
	Mobile.initialize(self,grid,80,x,y)

	self.sx = math.random(0,1)*2-1
	self.sy = math.random(0,1)*2-1
	self.vx = self.sx
	self.vy = self.sy
	
	
	self.x = self.x + self.sx
	self.y = self.y + self.sy
	self.grid_x = self.act_x + self.sx * Grid.size
	self.grid_y = self.act_y + self.sy * Grid.size
	
	self.sp = math.random(1,#Ubo.quads)
	self.nc = 0.1
	

end

function Ubo:draw() 
		lg.draw(Ubo.image,Ubo.quads[self.sp],self.act_x,self.act_y)
end

function Ubo:animate(dt)
		self.nc = self.nc - dt
		if self.nc < 0 then
			self.nc = 0.1
			self.sp = self.sp + 1
			if self.sp > #Ubo.quads then
				self.sp = 1
			end
		end

end

function Ubo:update(dt)

		Mobile.update(self,dt)
		if self:atdest() then
			
			local x = self.x + self.vx
			local y = self.y + self.vy
			if self.grid:isTail(x,y) then
				self.grid:setEvent(Grid.EVENT_UBO_HIT_TAIL)
				return
			elseif not self.grid:isCellFree(x,y) then
				local fx = self.grid:isCellFree(x,self.y)
				local fy = self.grid:isCellFree(self.x,y)
				if fx == fy then
					self.vx = - self.vx
					self.vy = - self.vy
				elseif fx then
					self.vy = - self.vy
				else
					self.vx = - self.vx
				end
				x = self.x + self.vx
				y = self.y + self.vy
			end
			self.sx = self.vx
			self.sy = self.vy
			self.x = x
			self.y = y
			self.grid_x = self.grid_x + self.vx * Grid.size
			self.grid_y = self.grid_y + self.vy * Grid.size
		end

end

function Ubo.static:load()

	local gsize = Grid.size
	local qid = love.image.newImageData(gsize*8,gsize)
	for x=0,qid:getWidth()-1 do
		for y=0,gsize-1 do
			qid:setPixel(x,y,0,0,0,0)
		end
	end


	for i=0,3 do
		local bsize = i
		local u = 128 + 32 + i * 32
		if u > 255 then
			u = 255
		end
		for x=0,gsize-1 do
			for y = 0,gsize-1 do
				if x >= bsize and x < (gsize - bsize) and y>=bsize and y <(gsize-bsize) then
					qid:setPixel(x + i*gsize,y,u/255,u/255,0,1)
					qid:setPixel(8*gsize -1 - x - i*gsize,y,u/255,u/255,0,1)
				end 
			end
		end
		
	end

	local img = lg.newImage(qid)
	Ubo.static.image = img
	Ubo.static.quads={}
	for i = 1,img:getWidth()/gsize do
		Ubo.static.quads[i] = lg.newQuad((i-1)*gsize,0,gsize,gsize,img:getWidth(),img:getHeight())
	end

end