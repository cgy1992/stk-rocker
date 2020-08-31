import os
import string
import threading
      
class GameInfo1vs1:
    def __init__(self, player, played_games, goals, elo):
        self.player = player
        self.played_games = played_games
        self.goals = goals
        self.elo = elo
        
    def __lt__(self, other):
        if (self.elo != other.elo):
            return self.elo > other.elo
        elif (self.goals != other.goals):
            return self.goals > other.goals
        elif (self.played_games != other.played_games):
            return self.played_games > other.played_games
        else:
            return self.player < other.player
        
    def __repr__(self):
        return "GameInfo(" + self.player+", "+str(self.played_games)+", "+str(self.goals) +", "+str(self.elo) + ")"


def update_stats():
    game_stats_file = open("game_stat_1vs1.txt")
    stats = []
    for line in game_stats_file:
        if line == "Player Played_Games Goals ELO\n":
            continue
        spl = line.split(" ")
        stats.append(GameInfo1vs1(spl[0],int(spl[1]),int(spl[2]),round(float(spl[3]),2)))
    stats = sorted(stats)
    game_stats_file.close()

    warnfile=open("warn.txt","w")
    warnfile.write("0")
    game_stats_file = open("game_stat_1vs1.txt",mode="w")
    game_stats_file.write("Player Played_Games Goals ELO\n")
    for s in stats:
        game_stats_file.write(s.player + " " + str(s.played_games) + " " + str(s.goals) + " " + str(s.elo)+ "\n")
    game_stats_file.close()
    warnfile.close()
    os.system("rm warn.txt")

def skript():
    update_stats()
    os.system("python3 updateHTML_1vs1.py")
    '''
    f=open("game_stat_1vs1.txt","r")
    g=open("old_game_stat_1vs1.txt","r")

    cwd=os.getcwd()
    lines=f.readlines()
    lines_old=g.readlines()
    player=[]
    player_old=[]
    goals=[]
    goals_old=[]
    games=[]
    games_old=[]
    elo=[]
    elo_old=[]
    for line in lines:
        line=str.split(line)
        player.append(line[0])
        games.append(line[1])
        goals.append(line[2])
        elo.append(line[3])
    for line in lines_old:
        line=str.split(line)
        player_old.append(line[0])
        games_old.append(line[1])
        goals_old.append(line[2])
        elo_old.append(line[3])
    
    os.chdir("../../../pywikibot")
    replace_old="| "+player_old[1]+" || "+games_old[1]+" || "+goals_old[1]+" || "+elo_old[1]
    replace="| "+player[1]+" || "+games[1]+" || "+goals[1]+" || "+elo[1]
    print(replace_old,replace)
    for i in range(2,len(lines_old)):
        replace_old+="\n|-\n| "+player_old[i]+" || "+games_old[i]+" || "+goals_old[i]+" || "+elo_old[i]
        replace+="\n|-\n| "+player[i]+" || "+games[i]+" || "+goals[i]+" || "+elo[i]
    for j in range(len(lines_old),len(lines)):
        replace+="\n|-\n| "+player[j]+" || "+games[j]+" || "+goals[j]+" || "+elo[j]
    print(replace)
    print(replace_old)
    os.system("python3 pwb.py replace -page:\"Rocker's 1 vs 1 Server\" \""+replace_old+"\"  \""+replace+"\" <inp.inp")
    os.chdir(cwd)
    os.system("cp game_stat_1vs1.txt old_game_stat_1vs1.txt")
    '''

threads = []
t = threading.Thread(target=skript)
threads.append(t)
t.start()
