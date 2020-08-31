import os
import string

class GameInfo:
    def __init__(self, team, points, game_points,ct_game_points,game_point_diff,goals,ct_goals,goal_diff):
        self.team = team
        self.points = points
        self.game_points = game_points
        self.ct_game_points = ct_game_points
        self.game_point_diff = game_point_diff
        self.goals = goals
        self.ct_goals = ct_goals
        self.goal_diff = goal_diff
        
    def __lt__(self, other):
        if (self.points != other.points):
            return self.points > other.points
        elif (self.game_point_diff != other.game_point_diff):
            return self.game_point_diff > other.game_point_diff
        elif (self.game_points != other.game_points):
            return self.game_points > other.game_points
        elif (self.goal_diff != other.goal_diff):
            return self.goal_diff > other.goal_diff
        elif (self.goals != other.goals):
            return self.goals > other.goals
        else:
            return self.team < other.team
        
    def __repr__(self):
        return "GameInfo(" + self.team+", "+str(self.points)+", "+str(self.game_points) +", "+str(self.ct_game_points)+", "+str(self.game_point_diff) + "," + str(self.goals) + "," + str(self.ct_goals) +", "+str(self.goal_diff) +")"
        
def update_stats():
    game_stats_file = open("supertournament_table.txt")
    stats = []

    for line in game_stats_file:
        if line == "Team Points GamePoints CounterGamePoints GamePointsDifference Goals CounterGoals GoalDifference\n":
            continue
        spl = line.split(" ")
        stats.append(GameInfo(spl[0], int(spl[1]), int(spl[2]), int(spl[3]), int(spl[4]), int(spl[5]), int(spl[6]), int(spl[7])))
    stats = sorted(stats)
    game_stats_file.close()

    game_stats_file = open("supertournament_table.txt", mode="w") #wenn es klappt zu w ändern
    game_stats_file.write("Team Points GamePoints CounterGamePoints GamePointsDifference Goals CounterGoals GoalDifference\n")
    for s in stats:
        game_stats_file.write(s.team + " " + str(s.points) + " " + str(s.game_points) + " " + str(s.ct_game_points) + " " + str(s.game_point_diff)  + " " + str(s.goals)+ " " + str(s.ct_goals) + " " + str(s.goal_diff) + "\n")
    game_stats_file.close()


f=open("supertournament_matchplan.txt","r")
g=open("supertournament_game.txt","r")
f2=open("new_matchplan","w")
lines=f.readlines()
glines=g.readlines()
home="singdrossel"
guest="ringdrossel"
if len(glines)>=2:
    home=str.split(glines[0])[0]
    guest=str.split(glines[0])[1]
    home_goals=str.split(glines[len(glines)-1])[0]
    guest_goals=str.split(glines[len(glines)-1])[1]
    game_number=len(glines)-1
for line in lines:
    line_split=str.split(line)
    if line_split[0]==home and line_split[1]==guest:
        f2.write(line_split[0]+' '+line_split[1]+' '+line_split[2]+' '+line_split[3]+' '+line_split[4]+' '+line_split[5])
        if game_number==1:
            f2.write(' '+home_goals+'-'+guest_goals+' '+line_split[7]+' '+line_split[8]+' ? ?\n')
        if game_number==2:
            f2.write(' '+line_split[6]+' '+home_goals+'-'+guest_goals+' '+line_split[8]+' ? ?\n')
        if game_number==3:
            f2.write(' '+line_split[6]+' '+line_split[7]+' '+home_goals+'-'+guest_goals)
            gp_home=0
            gp_guest=0
            home_goals=0
            guest_goals=0
            for i in range(1,4):
                gline=str.split(glines[i])
                home_goals+=int(gline[0])
                guest_goals+=int(gline[1])
                if int(gline[0])>=int(gline[1]): gp_home+=1
                if int(gline[0])<=int(gline[1]): gp_guest+=1
            winner="."
            if gp_home>gp_guest: winner=home
            elif gp_home<gp_guest: winner=guest
            else: winner="Draw"
            f2.write(" "+str(gp_home)+"-"+str(gp_guest)+" "+winner+"\n")
        continue
    f2.write(line)
f2.close()
os.system("mv new_matchplan supertournament_matchplan.txt")
#os.system("python3 sup_matchplan_online.py")

if game_number==3:
    f=open("supertournament_table.txt","r")
    f2=open("new_table","w")
    lines=f.readlines()
    for line in lines:
        lines_split=str.split(line)
        if lines_split[0]==home:
            new_points=1
            if winner==home: new_points=3
            if winner==guest: new_points=0  
            f2.write(home+" "+str(int(lines_split[1])+new_points)+" "+str(int(lines_split[2])+gp_home) +" "+str(int(lines_split[3])+gp_guest) + " "+str(int(lines_split[4])+gp_home-gp_guest)+ " "+str(int(lines_split[5])+int(home_goals)) + " "+str(int(lines_split[6])+int(guest_goals)) + " "+str(int(lines_split[7])+int(home_goals)-int(guest_goals)) + "\n" )
            continue
        if lines_split[0]==guest:
            new_points=1
            if winner==guest: new_points=3
            if winner==home: new_points=0      
            f2.write(guest+" "+str(int(lines_split[1])+new_points)+" "+str(int(lines_split[2])+gp_guest) +" "+str(int(lines_split[3])+gp_home) + " "+str(int(lines_split[4])+gp_guest-gp_home)+ " "+str(int(lines_split[5])+int(guest_goals)) + " "+str(int(lines_split[6])+int(home_goals)) + " "+str(int(lines_split[7])+int(guest_goals)-int(home_goals)) + "\n" )
            continue
        f2.write(line)
    f2.close()
    os.system("mv new_table supertournament_table.txt")
    update_stats()
    f=open("supertournament_game.txt","w")
    f.close()

