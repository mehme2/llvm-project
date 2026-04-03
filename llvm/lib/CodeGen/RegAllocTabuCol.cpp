#include "RegAllocGraphSolvers.h"
#include <vector>
#include "llvm/Support/CommandLine.h"

static llvm::cl::opt<uint32_t> IterationCount("tabucol-iterations", llvm::cl::init(10000), llvm::cl::Hidden, llvm::cl::desc("tabucol max iterations"));

class UndiGraph{
public:
    int vertexNum;
    std::vector<int>* edge;
};

//
// Created by Guan on 2020/9/14.
//

#define LNOISE_INIT 10

class TabuCol2{
public:
    TabuCol2(UndiGraph g,int colorNum,int maxIterationNum);
    void init();
    //solution, Assignment Representation
    int* sol;
    //vertex number in Grouping Representation, groupVertexNum[i] means the number of vertexes with color i
    int* groupVertexNum;
    //Evaluation Function=the num of conflicting edges
    int f;
    //for Aspiration
//    int* bestSol;
    int bestF;
    void tabuSearch();
    void setColrNum(int k){
        this->colorNumK=k;
    };
private:
    int colorNumK;
    UndiGraph g;
    //Number of Iterations
    int iter;
    //maximum number of iterations
    int maxIter;
    //Adjacent_Color_Table
    int** adjColTab;
    //the tabuTenure is changed to 1-d array,
    //0 represents non-tabu, a positive integer n means color n is tabu
    int* tabuTenure;
    //record the last iter of a move,
    //moveLastIter[i][j]=n means the last iter is n when changing vertex i to color j. init with 0.
    int** moveLastIter;
    struct Move{
        int vertex;
        //original color
        int oriCol;
        //next color
        int nextCol;
        int deltaF;
    }nextMove;
    void findMove();
    void setRandMove();
    //probability(0-100)
    //the probability of accepting one conflict zero move
    int ZNoise=20;
    //the probability of move at random
    int LNoise=LNOISE_INIT;
    //the probability of accepting SECOND_BEST_MOVE
    int SNoise=10;
    //the number of iterations that deltaF >=0, which is used to control LNoise
    int nonNegativeDeltaIter=0;
    void makeMove();
};

//
// Created by Guan on 2020/9/14.
//

#include <iostream>
#include <climits>
#include <cstring>

//using std::cout;
//using std::endl;

//init
TabuCol2::TabuCol2(UndiGraph graph,int colorNum,int maxIterationNum) {
    this->colorNumK=colorNum;
    this->g=graph;
    this->maxIter=maxIterationNum;
    f=0;
    //allocate memory
    sol=new int[g.vertexNum];
//    bestSol=new int[g.vertexNum];
    groupVertexNum=new int[colorNumK];
    memset(groupVertexNum,0, sizeof(int)*colorNumK);
    tabuTenure=new int[g.vertexNum];
    memset(tabuTenure, -1, sizeof(int)*g.vertexNum);
    adjColTab=new int*[g.vertexNum];
    moveLastIter=new int*[g.vertexNum];
    for(int i=0;i<g.vertexNum;++i){
        adjColTab[i]=new int[colorNumK];
        moveLastIter[i]=new int[colorNumK];
        memset(moveLastIter[i],0,sizeof(int)*colorNumK);
        memset(adjColTab[i], 0, sizeof(int)*colorNumK);
    }

    //select a solution randomly
    for(int i=0;i<g.vertexNum;++i){
        sol[i]=rand()%colorNum;
        ++groupVertexNum[sol[i]];
    }

    //init Adjacent_Color_Table & f
    int color_i;
    int color_j;
    for(int i=0;i<g.vertexNum;++i){
        color_i=sol[i];
        for(int j=0;j<g.edge[i].size();++j){
            color_j=sol[g.edge[i][j]];
            if(color_i==color_j){
                ++f;
            }
            ++adjColTab[i][color_j];
        }
    }
    f/=2;
    bestF=f;
}

void TabuCol2::tabuSearch() {
    iter=0;
    while(iter<maxIter){
        ++iter;
        findMove();
        makeMove();
        if(f==0){
            std::cout<<"Success"<<std::endl;
            //output result
//            for(int i=0;i<g.vertexNum;++i){
//                cout<<i+1<<' '<<sol[i]<<endl;
//            }
            //verify
            for(int i=0;i<g.vertexNum;++i){
                for(int j=0;j<g.edge[i].size();++j){
                    if(sol[i]==sol[g.edge[i][j]]){
                        std::cout<<"Error"<<std::endl;
                    }
                }
            }
            //output Number of Iterations
            std::cout<<"Number of Iterations: "<<iter<<std::endl;
            return;
        }
    }
    //cout<<"Not Found"<<endl;
}

void TabuCol2::setRandMove(){
    //move randomly
    nextMove.vertex=rand()%g.vertexNum;
    nextMove.oriCol=sol[nextMove.vertex];
    nextMove.nextCol=rand()%(colorNumK-1);
    if(nextMove.oriCol==nextMove.nextCol){
        nextMove.nextCol=colorNumK-1;
    }
    nextMove.deltaF=adjColTab[nextMove.vertex][nextMove.nextCol]-adjColTab[nextMove.vertex][sol[nextMove.vertex]];
}

void TabuCol2::findMove() {

    struct zeroMove{
        int Inew;
        int Iold;
        int Inew_Iold;
        int vertex;
        int color;
    }bestNoConflictZeroMove,bestOneConflictZeroMove;
    //init with -1
    bestNoConflictZeroMove.vertex=-1;
    bestNoConflictZeroMove.Inew=-1;
    bestOneConflictZeroMove.vertex=-1;
    bestOneConflictZeroMove.Inew_Iold=-99999999;
    //bestMove,secondBestMove are used when delataF>0; prmBestMove is used when delataF<0
    struct move{
        int deltaF;
        int vertex;
        int color;
    }bestMove,secondBestMove,prmBestMove;
    bestMove.deltaF=INT_MAX;
    secondBestMove.deltaF=INT_MAX;
    prmBestMove.deltaF=INT_MAX;
    prmBestMove.vertex=-1;
    //decide whether to move at random
    int randMove=rand()%100;


    int tmpDeltaF;



    //search promising moves(deltaF<0) ,𝑐𝑜𝑛𝑓𝑙𝑖𝑐𝑡 𝑧𝑒𝑟𝑜 𝑚𝑜𝑣𝑒 and deltaF>0 moves
    //if we find minus deltaF(promising move), we don't need to process moves with deltaF>=0

    for(int i=0;i<g.vertexNum;++i){
        //serch conflicting vertexes
        if(adjColTab[i][sol[i]] > 0){
            for(int k=0;k<colorNumK;++k){
                if(k!=sol[i]){
                    tmpDeltaF=adjColTab[i][k]-adjColTab[i][sol[i]];
                    if(tmpDeltaF<0){
                        if(tabuTenure[i]!=k or (f+tmpDeltaF)<bestF ){ //check if being banned
                            bool update=false;
                            if(tmpDeltaF<prmBestMove.deltaF){
                                //update prmBestMove
                                update=true;
                            }else if(tmpDeltaF==prmBestMove.deltaF){
                                if(moveLastIter[i][k] < moveLastIter[prmBestMove.vertex][prmBestMove.color]){
                                    //update prmBestMove
                                    update=true;
                                }else if(moveLastIter[i][k] == moveLastIter[prmBestMove.vertex][prmBestMove.color]){
                                    if(rand()%2==0){ //if =, update at random
                                        update=true;
                                    }
                                }
                            }
                            if(update){
                                prmBestMove.vertex=i;
                                prmBestMove.color=k;
                                prmBestMove.deltaF=tmpDeltaF;
                            }
                        }
                    }else if(prmBestMove.vertex == -1){ //prmBestMove.vertex!=-1 means minus deltaF move found. If we found minus DeltaF move, we don't need to process deltaF>=0
                        if(tmpDeltaF==0){
                            if(adjColTab[i][sol[i]]==1){ //one conflict zero move
                                if(tabuTenure[i]!=k){ //zero move also need to consider tabu !=k ==-1 two strate
                                    int Inew=groupVertexNum[k];
                                    int Inew_Iold=Inew-groupVertexNum[sol[i]];
                                    bool update=false;
                                    if(Inew_Iold > bestOneConflictZeroMove.Inew_Iold){
                                        //update bestOneConflictZeroMove
                                        update=true;
                                    }else if(Inew_Iold == bestOneConflictZeroMove.Inew_Iold){
                                        if(Inew > bestOneConflictZeroMove.Inew ){
                                            //update bestOneConflictZeroMove
                                            update=true;
                                        }else if(Inew == bestOneConflictZeroMove.Inew){
                                            if(moveLastIter[i][k] < moveLastIter[bestOneConflictZeroMove.vertex][bestOneConflictZeroMove.color]){
                                                //update bestOneConflictZeroMove
                                                update=true;
                                            }else if(moveLastIter[i][k] == moveLastIter[bestOneConflictZeroMove.vertex][bestOneConflictZeroMove.color]){
                                                if(rand()%2==0){ //if =, update at random
                                                    update=true;
                                                }
                                            }
                                        }
                                    }
                                    if(update==true){
                                        bestOneConflictZeroMove.vertex=i;
                                        bestOneConflictZeroMove.color=k;
                                        bestOneConflictZeroMove.Inew=Inew;
                                        bestOneConflictZeroMove.Inew_Iold=Inew_Iold;
                                    }
                                }
                            }
                        }else{ //tmpDeltaF>0
                            if(randMove>=LNoise){ //if move at random, we don't need to process delt>0
                                //not move at random
                                if(tabuTenure[i]!=k) { //check if being banned
                                    bool updateBest=false;
                                    bool updateSecond=false;
                                    if(tmpDeltaF<bestMove.deltaF){
                                        updateBest=true;
                                    }else if(tmpDeltaF==bestMove.deltaF){
                                        if(moveLastIter[i][k]<moveLastIter[bestMove.vertex][bestMove.color]){
                                            updateBest= true;
                                        }else if(moveLastIter[i][k]==moveLastIter[bestMove.vertex][bestMove.color]){
                                            if(rand()%2==0){
                                                updateBest= true;
                                            }
                                        }
                                    }else if(tmpDeltaF<secondBestMove.deltaF){
                                        updateSecond= true;
                                    }else if(tmpDeltaF==secondBestMove.deltaF){
                                        if(moveLastIter[i][k]<moveLastIter[secondBestMove.vertex][secondBestMove.color]){
                                            updateSecond= true;
                                        }else if(moveLastIter[i][k]==moveLastIter[secondBestMove.vertex][secondBestMove.color]){
                                            if(rand()%2==0){
                                                updateSecond= true;
                                            }
                                        }
                                    }
                                    if(updateBest){
                                        secondBestMove=bestMove;
                                        bestMove.vertex=i;
                                        bestMove.color=k;
                                        bestMove.deltaF=tmpDeltaF;
                                    }else if(updateSecond){
                                        secondBestMove.vertex=i;
                                        secondBestMove.color=k;
                                        secondBestMove.deltaF=tmpDeltaF;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    //select tmpDeltaF<0 moves
    if(prmBestMove.vertex!=-1){
//        printf("found <0\n");
        nextMove.vertex=prmBestMove.vertex;
        nextMove.oriCol=sol[prmBestMove.vertex];
        nextMove.deltaF=prmBestMove.deltaF;
        nextMove.nextCol=prmBestMove.color;
        return;
    }

    //search 𝑛𝑜 𝑐𝑜𝑛𝑓𝑙𝑖𝑐𝑡 𝑧𝑒𝑟𝑜 𝑚𝑜𝑣𝑒(no conflict &tmpDetaF=0)
    for(int i=0;i<g.vertexNum;++i){
        if(adjColTab[i][sol[i]] == 0){ //no conflict
            for(int k=0;k<colorNumK;++k){
                if(k!=sol[i]){
                    if(tabuTenure[i]==-1){ //zero move also need to consider tabu *
                        tmpDeltaF=adjColTab[i][k]-adjColTab[i][sol[i]];
                        if(tmpDeltaF==0){
                            int Inew=groupVertexNum[k];
                            int Iold=groupVertexNum[sol[i]];
                            bool update=false;
                            if(Inew>bestNoConflictZeroMove.Inew){
                                //update
                                update=true;
                            }else if(Inew==bestNoConflictZeroMove.Inew){
                                if(Iold<bestNoConflictZeroMove.Iold){
                                    //update
                                    update=true;
                                }else if(Iold==bestNoConflictZeroMove.Iold){
                                    if(moveLastIter[i][k] < moveLastIter[bestNoConflictZeroMove.vertex][bestNoConflictZeroMove.color]){
                                        //update
                                        update= true;
                                    }else if(moveLastIter[i][k] == moveLastIter[bestNoConflictZeroMove.vertex][bestNoConflictZeroMove.color]){
                                        if(rand()%2==0){
                                            //update
                                            update=true;
                                        }
                                    }
                                }
                            }
                            if(update){
                                bestNoConflictZeroMove.vertex=i;
                                bestNoConflictZeroMove.color=k;
                                bestNoConflictZeroMove.Inew=Inew;
                                bestNoConflictZeroMove.Iold=Iold;
                            }
                        }
                    }



                }
            }
        }
    }

    //select zero move
    bool skipZeroMove=false;
    if(bestNoConflictZeroMove.vertex!=-1 ){ // != -1 means found
        if(bestOneConflictZeroMove.vertex!= -1){
            //no conflict zero move and one conflict zero move are found
//            printf("found no conflict zero move %d %d and one conflict zero move %d %d\n",
//                    bestNoConflictZeroMove.vertex,bestNoConflictZeroMove.color,bestOneConflictZeroMove.vertex,bestOneConflictZeroMove.color);
            if( (rand()%100)<ZNoise ){
                //accept one conflict zero move
                nextMove.vertex=bestOneConflictZeroMove.vertex;
                nextMove.nextCol=bestOneConflictZeroMove.color;
                nextMove.oriCol=sol[nextMove.vertex];
                nextMove.deltaF=0;
            }else{
                //accept no conflict zero move
                nextMove.vertex=bestNoConflictZeroMove.vertex;
                nextMove.nextCol=bestNoConflictZeroMove.color;
                nextMove.oriCol=sol[nextMove.vertex];
                nextMove.deltaF=0;
            }

        }else{
//            printf("found only no conflict zero move %d %d\n",bestNoConflictZeroMove.vertex,bestNoConflictZeroMove.color);
            //only no conflict zero move is found
            nextMove.vertex=bestNoConflictZeroMove.vertex;
            nextMove.nextCol=bestNoConflictZeroMove.color;
            nextMove.oriCol=sol[nextMove.vertex];
            nextMove.deltaF=0;

        }

    }else{
        if(bestOneConflictZeroMove.vertex!= -1){
            //only one conflict zero move is found
//            printf("found only one conflict zero move\n");
            nextMove.vertex=bestOneConflictZeroMove.vertex;
            nextMove.nextCol=bestOneConflictZeroMove.color;
            nextMove.oriCol=sol[nextMove.vertex];
            nextMove.deltaF=0;

        }else{
            skipZeroMove=true;
        }
    }
    //used to prevent zero move circulation
    if(rand()%100<LNoise){
        skipZeroMove= true;
    }

    if(!skipZeroMove){
        return;
    }


    //select deltaF>0 moves
//    printf("found >0\n");
    if (randMove<LNoise){
        //move at random
        setRandMove();
    }else{
        //not move at random
        if(secondBestMove.deltaF==INT_MAX){
            printf("ERROR!!!\n");
        }
//        printf("%d %d\n",bestMove.deltaF,secondBestMove.deltaF);
        if( (rand()%100)<SNoise ){
            //accept second best move
            nextMove.vertex=secondBestMove.vertex;
            nextMove.nextCol=secondBestMove.color;
            nextMove.oriCol=sol[secondBestMove.vertex];
            nextMove.deltaF=secondBestMove.deltaF;
        }else{
            //accept best move
            nextMove.vertex=bestMove.vertex;
            nextMove.nextCol=bestMove.color;
            nextMove.oriCol=sol[bestMove.vertex];
            nextMove.deltaF=bestMove.deltaF;
        }
    }
}

void TabuCol2::makeMove() {
    //update solution ,f and tabuTenure
    sol[nextMove.vertex]=nextMove.nextCol;

    --groupVertexNum[nextMove.oriCol];
    ++groupVertexNum[nextMove.nextCol];
    f = f + nextMove.deltaF;
    if(f < bestF){
        bestF = f;
    }

    //the tabu policy is changed:
    //after making a move, the current move is tabu
    //the adjacent vertexes become non-tabu

    tabuTenure[nextMove.vertex]=nextMove.oriCol;
    for(int i=0;i<g.edge[nextMove.vertex].size();++i){
        tabuTenure[g.edge[nextMove.vertex][i]]=-1;
    }
    //Update the Adjacent_Color_Table
    for(int i=0;i<g.edge[nextMove.vertex].size();++i){

        --adjColTab[g.edge[nextMove.vertex][i]][nextMove.oriCol];
        ++adjColTab[g.edge[nextMove.vertex][i]][nextMove.nextCol];
    }

    //update moveLastIter
    moveLastIter[nextMove.vertex][nextMove.nextCol]=iter;

    //control LNoise
    if(nextMove.deltaF<0){
        //decrease LNoise
        LNoise=LNOISE_INIT;
        nonNegativeDeltaIter=0;
    }else{
        //increase LNoise
        ++nonNegativeDeltaIter;
//        LNoise=LNOISE_INIT+25*nonNegativeDeltaIter;
        LNoise+=20*nonNegativeDeltaIter;
    }
    //check debug
//    if(iter==13345 or iter==13346){
//        int f2=0;
//        for(int i=0;i<g.vertexNum;++i){
//            for(int j=0;j<g.edge[i].size();++j){
//                if(sol[i]==sol[g.edge[i][j]]){
//                    ++f2;
//                }
//            }
//        }
//        f2/=2;
//    }

}

REGALLOC_GRAPH_SOLVER(RegAllocTabuColSolver)
{
    unsigned VertCount = Graph.getVertexCount();

    std::vector<std::vector<int>> Edges(VertCount);
    
    for(unsigned VertIndexA = 0;
        VertIndexA < VertCount;
        ++VertIndexA)
    {
        for(unsigned VertIndexB = 0;
            VertIndexB < VertCount;
            ++VertIndexB)
        {
            if(Graph.hasEdge(VertIndexA, VertIndexB))
            {
                Edges[VertIndexA].push_back(VertIndexB);
                Edges[VertIndexB].push_back(VertIndexA);
            }
        }
    }

    UndiGraph UGraph;

    UGraph.vertexNum = VertCount;
    UGraph.edge = Edges.data();

    unsigned PhysCount = Graph.getPhysRegCount();
    TabuCol2 Col(UGraph, PhysCount, IterationCount);
    Col.tabuSearch();

    std::vector<float> Weights(VertCount);

    for(unsigned VertIndex = 0;
        VertIndex < VertCount;
        ++VertIndex)
    {
        if(Graph.isPhys(VertIndex))
        {
            Weights[VertIndex] = 10000000.0f;
        }
        else
        {
            unsigned VirtIndex = Graph.vertIndexToVirtIndex(VertIndex);
            if(Graph.isSpillable(VirtIndex))
            {
                Weights[VertIndex] = Graph.getWeight(VirtIndex);
            }
            else Weights[VertIndex] = 10000.0f;
        }
    }

    int RemovedVertices = 0;
    for(int ColorIndex = 0;
        ColorIndex < PhysCount;
        ++ColorIndex)
    {
        bool HasConflicts = true;
        while(HasConflicts)
        {
            HasConflicts = false;
            int VertexToRemove = -1;
            float HighestConflictWeight = 0;
            unsigned PhysIndex;
            for(PhysIndex = 0;
                PhysIndex < PhysCount;
                ++PhysIndex)
            {
                if(Col.sol[Graph.physIndexToVertIndex(PhysIndex)] == ColorIndex) break;
            }
            if(PhysIndex == PhysCount) continue;
            for(unsigned VertIndexA = 0;
                VertIndexA < VertCount;
                ++VertIndexA)
            {
                if(Col.sol[VertIndexA] != ColorIndex) continue;
                float ConflictWeight = 0;
                for(unsigned VertIndexB = 0;
                    VertIndexB < VertCount;
                    ++VertIndexB)
                {
                    if(VertIndexA == VertIndexB) continue;
                    if(!Graph.hasEdge(VertIndexA, VertIndexB)) continue;
                    if(Col.sol[VertIndexB] != ColorIndex) continue;
                    HasConflicts = true;
                    ConflictWeight += Weights[VertIndexB];
                }
                if(ConflictWeight > HighestConflictWeight)
                {
                    HighestConflictWeight = ConflictWeight;
                    VertexToRemove = VertIndexA;
                }
            }
            if(HasConflicts)
            {
                Col.sol[VertexToRemove] = -1;
                ++RemovedVertices;
            }
        }
    }

    unsigned VirtCount = Graph.getVirtRegCount();
    std::vector<int> Solution(VirtCount, -1);

    for(unsigned PhysIndex = 0;
        PhysIndex < PhysCount;
        ++PhysIndex)
    {
        int ColorIndex = Col.sol[PhysIndex];

        if(ColorIndex == -1) continue;

        for(unsigned VirtIndex = 0;
            VirtIndex < VirtCount;
            ++VirtIndex)
        {
            unsigned VertIndex = Graph.virtIndexToVertIndex(VirtIndex);
            if(Col.sol[VertIndex] == ColorIndex)
            {
                Solution[VirtIndex] = PhysIndex;
            }
        }
    }

    return Solution;
}
