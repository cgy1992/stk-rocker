import os
import string
import threading
import argparse

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
        
def update_stats():
    game_stats_file = open("game_stat.txt")
    stats = []

    for line in game_stats_file:
        if line == "Player Played_Games Goals\n":
            continue
        spl = line.split(" ")
        stats.append(GameInfo(spl[0], round(float(spl[1]),2), int(spl[2])))
    stats = sorted(stats)
    game_stats_file.close()

    game_stats_file = open("game_stat.txt", mode="w")
    game_stats_file.write("Player Played_Games Goals\n")
    for s in stats:
        game_stats_file.write(s.player + " " + str(s.played_games) + " " + str(s.goals) + "\n")
    game_stats_file.close()

def skript():
    update_stats()
    os.system("python3 updateHTML.py")
    '''
    f=open("game_stat.txt","r")
    g=open("old_game_stat.txt","r")
    cwd=os.getcwd()
    lines=f.readlines()
    lines_old=g.readlines()
    player=[]
    player_old=[]
    goals=[]
    goals_old=[]
    games=[]
    games_old=[]
    for line in lines:
        line=str.split(line)
        player.append(line[0])
        games.append(line[1])
        goals.append(line[2])
    for line in lines_old:
        line=str.split(line)
        player_old.append(line[0])
        games_old.append(line[1])
        goals_old.append(line[2])
    os.chdir("../../../pywikibot")
    replace_old="| "+player_old[1]+" || "+games_old[1]+" || "+goals_old[1]
    replace="| "+player[1]+" || "+games[1]+" || "+goals[1]
    print(replace_old,replace)
    for i in range(2,len(lines_old)):
        replace_old+="\n|-\n| "+player_old[i]+" || "+games_old[i]+" || "+goals_old[i]
        replace+="\n|-\n| "+player[i]+" || "+games[i]+" || "+goals[i]
    for j in range(len(lines_old),len(lines)):
        replace+="\n|-\n| "+player[j]+" || "+games[j]+" || "+goals[j]
    print(replace)
    print(replace_old)
    os.system("python3 pwb.py replace -page:\"Rocker's 3 vs 3 Server\" \""+replace_old+"\"  \""+replace+"\" <inp.inp")
    os.chdir(cwd)
    os.system("cp game_stat.txt old_game_stat.txt")
    '''

threads = []
t = threading.Thread(target=skript)
threads.append(t)
t.start()
