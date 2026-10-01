WindowWidth = 800
WindowHeight = 400
love.load = function()
	love.window.setMode(WindowWidth, WindowHeight)
end

PosX = 400
PosY = 0
CharacterSize = 20
SpeedX = 120

VelocityY = 0
Gravity = -200
JumpForce = 150
WasPressingStomp = false
HasStomp = false
StompForce = -320
LifePoints = 3
BlinkTimer = 0
Points = 0
BestCombo = 0
Multiplier = 1

Enemies = {}
SpawnRate = 1
SpawnTimer = 0
EnemySize = 15
EnemyMinSpeed,EnemyMaxSpeed = 90 , 180

Labels = {}
LabelDisplayTime = 1
LabelSpeed = 30
BestScore = "Loading"

WasPressingReset = false
reset = function() -- dirty compressed reset
	PosX,PosY,VelocityY,HasStomp,WasPressingStomp,LifePoints,BlinkTimer,Points,BestCombo,Multiplier,Enemies,Labels = 400,0,0,false,false,3,0,0,0,1,{},{}
end

love.update = function(dt)
	if love.keyboard.isDown("escape") then love.event.push('quit') end
	if not WasPressingReset and love.keyboard.isDown("r") then reset() end
	WasPressingReset = love.keyboard.isDown("r")

	if LifePoints > 0 then
		local directionX = 0
		if love.keyboard.isDown("right") then directionX = 1 end
		if love.keyboard.isDown("left") then directionX = directionX - 1 end -- If the player is pressing both, the character dont move
		PosX = PosX + directionX * SpeedX * dt
		PosX = math.min(math.max(PosX,0), WindowWidth - CharacterSize) -- Clamping the character into the window

		VelocityY = VelocityY + Gravity * dt
		local isOnGround = PosY <= 1
		if isOnGround then
			HasStomp = false
			Multiplier = 1
			if love.keyboard.isDown("up") then
				VelocityY = JumpForce
			end
		elseif love.keyboard.isDown("down") and not WasPressingStomp and not HasStomp then
			VelocityY = StompForce
			HasStomp = true
		end
		WasPressingStomp = love.keyboard.isDown("down")
		PosY = PosY + VelocityY * dt
		PosY = math.max(PosY , 0)
		updateEnemies(dt)
		local hasHit = false
		for i = #Enemies,1,-1 do
			if rectangleCollision(PosX,PosY,CharacterSize,CharacterSize,Enemies[i].posX,Enemies[i].posY,EnemySize,EnemySize) then
				removeElement(Enemies,i)
				VelocityY = JumpForce
				if HasStomp then
					Points = Points + Multiplier
					Labels[#Labels + 1] = {Text = tostring(Multiplier) , posX = PosX, posY = PosY, time = LabelDisplayTime}
					BestCombo = math.max(BestCombo,Multiplier)
					Multiplier = Multiplier + 1
				else
					LifePoints = LifePoints - 1
					BlinkTimer = 1
					Multiplier = 1
					if LifePoints == 0 then onGameOver() end
				end
				hasHit = true
			end
		end
		if hasHit then HasStomp = false end
		if BlinkTimer > 0 then BlinkTimer = BlinkTimer - dt end
		for i= #Labels,1,-1 do
			Labels[i].time = Labels[i].time - dt
			if Labels[i].time <= 0 then removeElement(Labels,i) end
		end
	end
end

love.draw = function()
	love.graphics.setColor(255,255,20)
	love.graphics.print("Left/Right to move | Up to jump | Down to stomp you'll crush only if you stomp",305,15)
	if LifePoints > 0 then
		love.graphics.print("Life   : " .. tostring(LifePoints) , 15,15)
		love.graphics.print("Points : " .. tostring(Points) .. " Multiplier : " .. tostring(Multiplier) .. " Best Combo : " .. BestCombo, 15 , 35)
		if not (BlinkTimer > 0) or (math.sin(BlinkTimer * math.pi * 15) > .5) then
			if HasStomp then
				love.graphics.setColor(240,240,0)
			else
				love.graphics.setColor(240,240,75)
			end
			local x,y = getPositionFromCamera(PosX,PosY)
			love.graphics.rectangle("fill",x, y - CharacterSize,CharacterSize,CharacterSize)
		end
		love.graphics.setColor(255,65,40)
		for i,enemy in ipairs(Enemies) do
			local x,y = getPositionFromCamera(enemy.posX,enemy.posY)
			love.graphics.rectangle("fill",x,y - EnemySize,EnemySize,EnemySize)
		end
		love.graphics.setColor(25,128,255)
		for i,label in ipairs(Labels) do
			local x,y = getPositionFromCamera(label.posX,label.posY + LabelSpeed * (1 - label.time / LabelDisplayTime) + CharacterSize + EnemySize)
			love.graphics.print(label.Text,x,y)
		end
	else
		love.graphics.print("Congratulation you achieved : " .. tostring(Points) .. " Points! \nYour best combo is " .. BestCombo , 300,185)
		love.graphics.print("Best score : " .. tostring(BestScore), 335,220)
		love.graphics.print("Press R to restart", 375,240)
		love.graphics.print("The 200 lines of codes for this game are here : https://github.com/Redoxee/CompressedGame/blob/master/Game/main.lua",4,380)
	end
end

getPositionFromCamera = function(x,y)
	return x, y*-1 + WindowHeight
end

updateEnemies = function(dt)
	SpawnTimer = SpawnTimer - dt
	if SpawnTimer < 0 then
		SpawnTimer = SpawnRate
		local goRight = math.random() > .5
		Enemies[#Enemies + 1] = {
			posY = math.random(0,50),
			posX = goRight and -EnemySize or WindowWidth ,
			speed = math.random(EnemyMinSpeed,EnemyMaxSpeed) * (goRight and 1 or -1),
		}
	end
	for i = #Enemies, 1, -1 do
		local enemy = Enemies[i]
		enemy.posX = enemy.posX + enemy.speed * dt 
 		if ((enemy.speed > 0) and enemy.posX > WindowWidth) or (not (enemy.speed > 0) and enemy.posX < -EnemySize) then
 			removeElement(Enemies,i)
 		end 
 	end
end

removeElement = function(list,index)
	for i = index,#list do
		list[i] = list[i + 1]
	end
end

rectangleCollision = function(x1,y1,width1,height1,x2,y2,width2,height2) -- Simple aabb function
	local isNotColliding = x1 > (x2 + width2) or (x2 > x1 + width1) or y1 > (y2 + height2) or y2 > (y1 + height1)
	return not isNotColliding
end

onGameOver = function()
	if BestScore == "Loading" or BestScore ~= "Can't load" then
		local http = require("socket.http")
		local b, c, h = http.request("http://antonroy.fr/content/CompressedGame/php/HighScore.php?Score=" .. tostring(Points).."&BestCombo="..tostring(BestCombo))
		if c == 200 then BestScore = b else BestScore = "Can't load" end
	end
end
--[[<?php -- Server Side -- yes it is very easy to cheat but I trust you not to
	$host = "stub";
	$db = "stub";
	$dsn = 'stub'.$db.'stub'.$host;
	$user = "stub";
	$pwd = "stub";
	$pdo = new PDO($dsn,$user,$pwd);
	$pdo = new PDO($dsn,$user,$pwd);
	if (isset($_GET["Score"]) && isset($_GET["BestCombo"])){
		$sql = 'SELECT * FROM HighScore WHERE id = 1;';
		$rep = $pdo->query($sql);
		$rep = $rep->fetch();
		$_GET["Score"] = max(htmlspecialchars ($_GET["Score"]),$rep['Score']);
		$_GET["BestCombo"] = max(htmlspecialchars ($_GET["BestCombo"]),$rep['BestCombo']);
		$sql = 'UPDATE HighScore SET Score='.$_GET["Score"].', BestCombo = '.$_GET["BestCombo"].' WHERE id = 1;';
		$pdo->exec($sql);
	}
	$sql = 'SELECT Score,BestCombo FROM HighScore;';
	$rep = $pdo->query($sql);
	foreach ($rep as $row) {
		echo $row['Score'].", ".$row['BestCombo'];
	}
?>--]]
--[[function love.conf(t) -- conf.lua
    t.version = "0.10.0"                -- The LÖVE version this game was made for (string)
    t.window.title = "CompressedGame"        -- The window title (string)
    t.window.resizable = false         -- Let the window be user-resizable (boolean)
end--]]
-- Thanks for reading :)
-- Anton Roy Apache License 2.0 2016