

class GameInfo:
    def __init__(self, player, played_games, goals):
        self.player = player
        self.played_games = played_games
        self.goals = goals
        
    def __lt__(self, other):
        if (self.goals != other.goals):
            return self.goals > other.goals
        elif (self.played_games != other.played_games):
            return self.played_games > other.played_games
        else:
            return self.player < other.player
        
    def __repr__(self):
        return "GameInfo(" + self.player+", "+str(self.played_games)+", "+str(self.goals) + ")"
        
"""
a = GameInfo("TheRocker", 10, 73)
b = GameInfo("Waldnestvogel", 4, 14)
c = GameInfo("pingiibox-player-relay2", 3, 11)
d = GameInfo("HelgoIandGBLS", 3, 11)

arr = sorted([a, b, c, d])
print(arr)
"""

game_stats_file = open("game_stat.txt")
stats = []

for line in game_stats_file:
    if line == "Player Played_Games Goals\n":
        continue
    spl = line.split(" ")
    stats.append(GameInfo(spl[0], int(spl[1]), int(spl[2])))
    
stats = sorted(stats)
game_stats_file.close()

game_stats_file = open("game_stat.txt", mode="w")
for s in stats:
    game_stats_file.write(s.player + " " + str(s.played_games) + " " + str(s.goals) + "\n")
game_stats_file.close()


