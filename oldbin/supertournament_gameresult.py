import argparse
import string
import os

class GameInfo:
    def __init__(self, player, team, played_games, goals):
        self.player = player
        self.team = team
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
        return "GameInfo(" + self.player+", "+ self.team+", "+str(self.played_games)+", "+str(self.goals) + ")"
        
def update_stats():
    game_stats_file = open("supertournament_scorerlist.txt")
    stats = []

    for line in game_stats_file:
        if line == "Player Team Played_Games Goals\n":
            continue
        spl = line.split(" ")
        stats.append(GameInfo(spl[0],spl[1],int(spl[2]), int(spl[3])))
    stats = sorted(stats)
    game_stats_file.close()

    game_stats_file = open("supertournament_scorerlist.txt", mode="w")
    game_stats_file.write("Player Team Played_Games Goals\n")
    for s in stats:
        game_stats_file.write(s.player + " " + s.team + " " + str(s.played_games) + " " + str(s.goals) + "\n")
    game_stats_file.close()

parser = argparse.ArgumentParser()
parser.add_argument("red_team")
parser.add_argument("blue_team")
args = parser.parse_args()
red_team=args.red_team
blue_team=args.blue_team

f=open("supertournament_currentplayers.txt","r")
lines=f.readlines()
f2=open("supertournament_game.txt","a")
team1=red_team
team2=blue_team

test=open("supertournament_game.txt","r")
l=test.readlines()
last_game=0
if len(l)==3:last_game=1
new=1
if len(l)!=0:
    l[0]=str.split(l[0])
    if len(l[0])==2:
        new=0
if new==1:
    f2.write(team1+" "+team2+"\n")
test.close()

players=[]
goals=[]
teams=[]
team1_goals=0
team2_goals=0
for line in lines:
    if team1==str.split(line)[2]:
        team1_goals+=int(str.split(line)[1])
    if team2==str.split(line)[2]:
        team2_goals+=int(str.split(line)[1])
    players.append(str.split(line)[0])
    goals.append(str.split(line)[1])
    teams.append(str.split(line)[2])
    
f2.write(str(team1_goals)+" "+str(team2_goals)+"\n")
f2.close()

i=1
while i<len(players):
    f3=open("tmpgr_super","w")
    f3_read=open("supertournament_scorerlist.txt","r")
    lines3=f3_read.readlines()
    found=0
    for line in lines3:
        if str.split(line)[0]==players[i]:
            f3.write(players[i]+" "+teams[i]+" "+str(int(str.split(line)[2])+1)+" "+str(int(str.split(line)[3])+int(goals[i]))+"\n")
            found=1
            continue
        f3.write(line)
    if found==0: f3.write(players[i]+" "+teams[i]+" "+str(1)+" "+str(int(goals[i]))+"\n")
    f3.close()
    os.system("mv tmpgr_super supertournament_scorerlist.txt")
    i+=1
update_stats()
f=open("supertournament_currentplayers.txt","w")
f.write("Player Goals Team\n")
f.close()
os.system("python3 supertournament_update_matchplan.py")
