#include "sortParallel.hpp"

#include <optional>
#include <iomanip>
#include <iostream>
#include <fstream>

#include "config.hpp"
#include "spdlog/sinks/basic_file_sink.h"

size_t roundCoordDown(double coord)
{
    return (size_t)(coord + 0.25);
}

bool& accessStateArray(py::EigenDRef<Eigen::Array<bool, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>& stateArray,
    size_t i, size_t j, bool rowFirst)
{
    if(rowFirst)
    {
        return stateArray(i,j);
    }
    else
    {
        return stateArray(j,i);
    }
}

void fillDimensionDependantData(py::EigenDRef<Eigen::Array<bool, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>& stateArray, 
    size_t compZone[4], bool rowFirst, size_t outerDimCompZone[2], size_t innerDimCompZone[2],
    size_t& outerSize, size_t& innerSize, unsigned int& outerAODLimit, unsigned int& innerAODLimit)
{
    if(rowFirst)
    {
        outerDimCompZone[0] = compZone[0];
        outerDimCompZone[1] = compZone[1];
        innerDimCompZone[0] = compZone[2];
        innerDimCompZone[1] = compZone[3];
        outerSize = stateArray.rows();
        innerSize = stateArray.cols();
        outerAODLimit = AOD_ROW_LIMIT;
        innerAODLimit = AOD_COL_LIMIT;
    }
    else
    {
        outerDimCompZone[0] = compZone[2];
        outerDimCompZone[1] = compZone[3];
        innerDimCompZone[0] = compZone[0];
        innerDimCompZone[1] = compZone[1];
        outerSize = stateArray.cols();
        innerSize = stateArray.rows();
        outerAODLimit = AOD_COL_LIMIT;
        innerAODLimit = AOD_ROW_LIMIT;
    }
}

double inline costPerSubMove(double dist)
{
    return MOVE_COST_OFFSET_SUBMOVE + MOVE_COST_SCALING_LINEAR * dist + (MOVE_COST_SCALING_SQRT != 0 ? MOVE_COST_SCALING_SQRT * sqrt(dist) : 0);
}

ParallelMove ParallelMove::fromStartAndEnd(ParallelMove::Step start, ParallelMove::Step end, 
    std::shared_ptr<spdlog::logger> logger)
{
    ParallelMove move;
    ParallelMove::Step step1;
    ParallelMove::Step step2;

    step1.rowSelection = start.rowSelection;
    step1.colSelection = start.colSelection;
    step2.rowSelection = end.rowSelection;
    step2.colSelection = end.colSelection;

    double maxRowDist = 0;
    double maxColDist = 0;

    for(size_t i = 0; i < start.rowSelection.size(); i++)
    {
        if(end.rowSelection[i] > start.rowSelection[i])
        {
            step1.rowSelection[i] += 0.5;
            step2.rowSelection[i] -= 0.5;
        }
        else if(end.rowSelection[i] < start.rowSelection[i])
        {
            step1.rowSelection[i] -= 0.5;
            step2.rowSelection[i] += 0.5;
        }
        else
        {
            step1.rowSelection[i] += 0.5;
            step2.rowSelection[i] += 0.5;
        }
        double dist = abs(step1.rowSelection[i] - step2.rowSelection[i]);
        if(dist > maxRowDist)
        {
            maxRowDist = dist;
        }
    }
    for(size_t i = 0; i < start.colSelection.size(); i++)
    {
        if(end.colSelection[i] > start.colSelection[i])
        {
            step1.colSelection[i] += 0.5;
            step2.colSelection[i] -= 0.5;
        }
        else if(end.colSelection[i] < start.colSelection[i])
        {
            step1.colSelection[i] -= 0.5;
            step2.colSelection[i] += 0.5;
        }
        else
        {
            step1.colSelection[i] += 0.5;
            step2.colSelection[i] += 0.5;
        }
        double dist = abs(step1.colSelection[i] - step2.colSelection[i]);
        if(dist > maxColDist)
        {
            maxColDist = dist;
        }
    }
       
    move.steps.push_back(start);
    if(maxRowDist > DOUBLE_EQUIVALENCE_THRESHOLD || maxColDist > DOUBLE_EQUIVALENCE_THRESHOLD)
    {
        move.steps.push_back(step1);
        if(maxRowDist > DOUBLE_EQUIVALENCE_THRESHOLD && maxColDist > DOUBLE_EQUIVALENCE_THRESHOLD)
        {
            ParallelMove::Step intermediateStep;
            intermediateStep.rowSelection = step2.rowSelection;
            intermediateStep.colSelection = step1.colSelection;
            move.steps.push_back(std::move(intermediateStep));
        }
        move.steps.push_back(std::move(step2));
    }
    move.steps.push_back(end);
    return move;
}

double ParallelMove::cost()
{
    double cost = MOVE_COST_OFFSET;
    const auto& lastStep = this->steps[0];
    for(size_t step = 1; step < this->steps.size(); step++)
    {
        const auto& newStep = this->steps[step];
        unsigned int longestColDist = 0;
        for(size_t tone = 0; tone < lastStep.colSelection.size() && tone < newStep.colSelection.size(); tone++)
        {
            unsigned int dist = abs((int)(lastStep.colSelection[tone]) - (int)(newStep.colSelection[tone]));
            if(dist > longestColDist)
            {
                longestColDist = dist;
            }
        }
        unsigned int longestRowDist = 0;
        for(size_t tone = 0; tone < lastStep.rowSelection.size() && tone < newStep.rowSelection.size(); tone++)
        {
            unsigned int dist = abs((int)(lastStep.rowSelection[tone]) - (int)(newStep.rowSelection[tone]));
            if(dist > longestRowDist)
            {
                longestRowDist = dist;
            }
        }
        if(longestColDist == 0 || longestRowDist == 0)
        {
            cost += costPerSubMove(longestRowDist + longestColDist);
        }
        else if(abs((int)longestColDist - (int)longestRowDist) < DOUBLE_EQUIVALENCE_THRESHOLD)
        {
            cost += costPerSubMove(longestRowDist * M_SQRT2);
        }
        else
        {
            cost += costPerSubMove(sqrt(longestRowDist * longestRowDist + longestColDist * longestColDist));
        }
    }
    return cost;
}

bool ParallelMove::execute(py::EigenDRef<Eigen::Array<bool, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>> &stateArray, std::shared_ptr<spdlog::logger> logger)
{
    const auto& firstStep = this->steps.front();
    const auto& lastStep = this->steps.back();

    Eigen::Array<bool, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> stateArrayCopy = stateArray;

    if(firstStep.colSelection.size() != lastStep.colSelection.size() || firstStep.rowSelection.size() != lastStep.rowSelection.size())
    {
        logger->error("Move does not have equally many tones at beginning and end");
        return false;
    }

    std::stringstream startCols;
    std::stringstream endCols;
    std::stringstream startRows;
    std::stringstream endRows;
    for(const auto& col : firstStep.colSelection)
    {
        startCols << col << " ";
    }
    for(const auto& col : lastStep.colSelection)
    {
        endCols << col << " ";
    }
    for(const auto& row : firstStep.rowSelection)
    {
        startRows << row << " ";
    }
    for(const auto& row : lastStep.rowSelection)
    {
        endRows << row << " ";
    }
    logger->debug("Executing move, cols: ({})->({}), rows: ({})->({})", 
        startCols.str(), endCols.str(), startRows.str(), endRows.str());

    for(size_t rowTone = 0; rowTone < firstStep.rowSelection.size(); rowTone++)
    {
        for(size_t colTone = 0; colTone < firstStep.colSelection.size(); colTone++)
        {
            stateArrayCopy(roundCoordDown(firstStep.rowSelection[rowTone]),
                roundCoordDown(firstStep.colSelection[colTone])) = false;
        }
    }
    for(size_t rowTone = 0; rowTone < firstStep.rowSelection.size(); rowTone++)
    {
        for(size_t colTone = 0; colTone < firstStep.colSelection.size(); colTone++)
        {
            if(stateArray(roundCoordDown(firstStep.rowSelection[rowTone]),
                roundCoordDown(firstStep.colSelection[colTone])))
            {
                stateArrayCopy(roundCoordDown(lastStep.rowSelection[rowTone]),
                    roundCoordDown(lastStep.colSelection[colTone])) = true;
            }
        }
    }
    stateArray = stateArrayCopy;
    return true;
}

std::tuple<std::optional<ParallelMove>,int,double> fillColumnHorizontally(py::EigenDRef<Eigen::Array<bool, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>& stateArray, 
    size_t compZone[4], std::shared_ptr<spdlog::logger> logger)
{
    double halfStepMoveCost = costPerSubMove(M_SQRT1_2);
    std::optional<ParallelMove> bestMove = std::nullopt;
    unsigned int bestFillableGaps = 0;
    double bestCostPerFilledGap = 0;
    for(bool rowFirst : {true, false})
    {
        if((rowFirst && !ALLOW_MOVES_BETWEEN_ROWS) || (!rowFirst && !ALLOW_MOVES_BETWEEN_COLS))
        {
            continue;
        }
        size_t rowDimCompZone[2];
        size_t colDimCompZone[2];
        size_t rows;
        size_t cols;
        unsigned int rowAODLimit = 0;
        unsigned int colAODLimit = 0;
        fillDimensionDependantData(stateArray, compZone, rowFirst, rowDimCompZone, colDimCompZone, rows, cols, rowAODLimit, colAODLimit);

        unsigned int aodLimit = rowAODLimit < AOD_TOTAL_LIMIT ? rowAODLimit : AOD_TOTAL_LIMIT;

        std::vector<std::vector<size_t>> emptyCompZoneLocations;
        for(size_t j = colDimCompZone[0]; j < colDimCompZone[1]; j++)
        {
            std::vector<size_t> emptySitesInCol;
            for(size_t i = rowDimCompZone[0]; i < rowDimCompZone[1]; i++)
            {
                if(!accessStateArray(stateArray, i, j, rowFirst))
                {
                    emptySitesInCol.push_back(i);
                }
            }
            emptyCompZoneLocations.push_back(std::move(emptySitesInCol));
        }

        for(size_t borderColTemp = 0; borderColTemp < cols - (colDimCompZone[1] - colDimCompZone[0]); borderColTemp++)
        {
            size_t borderCol = borderColTemp;
            size_t borderAtomsIndex = borderColTemp;
            if(borderColTemp >= colDimCompZone[0])
            {
                borderCol += (colDimCompZone[1] - colDimCompZone[0]);
                borderAtomsIndex -= colDimCompZone[0];
            }
            std::vector<size_t> atomLocations;

            ParallelMove::Step start;
            if(rowFirst)
            {
                start.colSelection.push_back(borderCol);
            }
            else
            {
                start.rowSelection.push_back(borderCol);
            }

            for(size_t i = 0; i < rows; i++)
            {
                if(accessStateArray(stateArray, i, borderCol, rowFirst))
                {
                    atomLocations.push_back(i);
                }
            }

            for(size_t targetCol = colDimCompZone[0]; targetCol < colDimCompZone[1]; targetCol++)
            {
                ParallelMove::Step end;
                if(rowFirst)
                {
                    end.colSelection.push_back(targetCol);
                }
                else
                {
                    end.rowSelection.push_back(targetCol);
                }

                std::vector<size_t> atomLocationsCopy = atomLocations;
                const auto& emptySitesInCol = emptyCompZoneLocations[targetCol - colDimCompZone[0]];
                size_t *sourceSiteMapping = new size_t[emptySitesInCol.size()]();
                unsigned int filledSites = 0;
                size_t dist = 1;
                for(; dist < cols; dist++)
                {
                    if(rowFirst)
                    {
                        start.rowSelection.clear();
                        end.rowSelection.clear();
                    }
                    else
                    {
                        start.colSelection.clear();
                        end.colSelection.clear();
                    }
                    size_t atomLocationIndex = 0;
                    filledSites = 0;
                    for(const auto& emptySite : emptySitesInCol)
                    {
                        while(atomLocationIndex < atomLocations.size() && (int)atomLocations[atomLocationIndex] < (int)emptySite - (int)dist)
                        {
                            atomLocationIndex++;
                        }
                        if(atomLocationIndex >= atomLocations.size() || filledSites == aodLimit)
                        {
                            break;
                        }
                        else if((int)atomLocations[atomLocationIndex] >= (int)emptySite - (int)dist && 
                            atomLocations[atomLocationIndex] <= emptySite + dist)
                        {
                            if(rowFirst)
                            {
                                start.rowSelection.push_back(atomLocations[atomLocationIndex]);
                                end.rowSelection.push_back(emptySite);
                            }
                            else
                            {
                                start.colSelection.push_back(atomLocations[atomLocationIndex]);
                                end.colSelection.push_back(emptySite);
                            }
                            filledSites++;
                            atomLocationIndex++;
                        }
                    }
                    double approxCost = MOVE_COST_OFFSET + costPerSubMove(dist - 1) + 
                        costPerSubMove(abs((int)borderCol - (int)targetCol) - 1) + 2 * halfStepMoveCost;
                    if(!bestMove.has_value() || approxCost / filledSites < bestCostPerFilledGap)
                    {
                        ParallelMove move = ParallelMove::fromStartAndEnd(start, end, logger);
                        double costPerGap = move.cost() / filledSites;
                        if(!bestMove.has_value() || costPerGap < bestCostPerFilledGap)
                        {
                            bestMove = move;
                            bestCostPerFilledGap = costPerGap;
                            bestFillableGaps = filledSites;
                        }
                    }
                    if(filledSites == emptySitesInCol.size() || filledSites == atomLocations.size() || filledSites >= aodLimit)
                    {
                        break;
                    }
                }
                delete[] sourceSiteMapping;
            }
        }
    }
    logger->info("Best horizontal column move can fill {} at a cost of {} per gap", bestFillableGaps, bestCostPerFilledGap);
    return std::tuple<std::optional<ParallelMove>,int,double>(bestMove, bestFillableGaps, bestCostPerFilledGap);
}

std::tuple<std::optional<ParallelMove>,int,double> fillRowThroughSubspace(py::EigenDRef<Eigen::Array<bool, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>& stateArray, 
    size_t compZone[4], std::shared_ptr<spdlog::logger> logger)
{
    double halfStepMoveCost = costPerSubMove(M_SQRT1_2);
    std::optional<ParallelMove> bestMove = std::nullopt;
    unsigned int bestFillableGaps = 0;
    double bestCostPerFilledGap = 0;
    for(bool rowFirst : {true, false})
    {
        if((rowFirst && !ALLOW_MOVES_BETWEEN_ROWS) || (!rowFirst && !ALLOW_MOVES_BETWEEN_COLS))
        {
            continue;
        }
        size_t outerDimCompZone[2];
        size_t innerDimCompZone[2];
        size_t outerSize;
        size_t innerSize;
        unsigned int innerAODLimit = 0;
        unsigned int outerAODLimit = 0;
        fillDimensionDependantData(stateArray, compZone, rowFirst, outerDimCompZone, innerDimCompZone, outerSize, innerSize, outerAODLimit, innerAODLimit);

        unsigned int *borderAtomsLeft = new (std::nothrow) unsigned int[outerDimCompZone[1] - outerDimCompZone[0]]();
        if(borderAtomsLeft == 0)
        {
            logger->error("Could not allocate memory");
            return std::tuple<std::optional<ParallelMove>,int,double>(std::nullopt, 0, __DBL_MAX__);
        }
        unsigned int *borderAtomsRight = new (std::nothrow) unsigned int[outerDimCompZone[1] - outerDimCompZone[0]]();
        if(borderAtomsRight == 0)
        {
            logger->error("Could not allocate memory");
            return std::tuple<std::optional<ParallelMove>,int,double>(std::nullopt, 0, __DBL_MAX__);
        }
        unsigned int *emptyCompZoneLocations = new (std::nothrow) unsigned int[outerDimCompZone[1] - outerDimCompZone[0]]();
        if(emptyCompZoneLocations == 0)
        {
            logger->error("Could not allocate memory");
            return std::tuple<std::optional<ParallelMove>,int,double>(std::nullopt, 0, __DBL_MAX__);
        }
        for(size_t i = outerDimCompZone[0]; i < outerDimCompZone[1]; i++)
        {
            for(size_t j = 0; j < innerSize; j++)
            {
                if(accessStateArray(stateArray, i, j, rowFirst))
                {
                    if(j < innerDimCompZone[0])
                    {
                        borderAtomsLeft[i - outerDimCompZone[0]]++;
                    }
                    else if(j >= innerDimCompZone[1])
                    {
                        borderAtomsRight[i - outerDimCompZone[0]]++;
                    }
                }
                else if(j >= innerDimCompZone[0] && j < innerDimCompZone[1])
                {
                    emptyCompZoneLocations[i - outerDimCompZone[0]]++;
                }
            }
        }
        
        for(size_t iBorder = outerDimCompZone[0]; iBorder < outerDimCompZone[1]; iBorder++)
        {
            const unsigned int& currentBorderAtomsLeft = borderAtomsLeft[iBorder - outerDimCompZone[0]];
            const unsigned int& currentBorderAtomsRight = borderAtomsRight[iBorder - outerDimCompZone[0]];
            for(size_t iTarget = outerDimCompZone[0]; iTarget < outerDimCompZone[1]; iTarget++)
            {
                unsigned int outerDist = abs((int)iBorder - (int)iTarget);
                if(outerDist > 0)
                {
                    outerDist--;
                }
                const unsigned int& currentEmptyCompZoneLocations = emptyCompZoneLocations[iTarget - outerDimCompZone[0]];

                unsigned int fillableGaps = (currentBorderAtomsLeft + currentBorderAtomsRight) < currentEmptyCompZoneLocations ? 
                    (currentBorderAtomsLeft + currentBorderAtomsRight) : currentEmptyCompZoneLocations;
                if(fillableGaps > innerAODLimit)
                {
                    fillableGaps = innerAODLimit;
                }
                if(fillableGaps > AOD_TOTAL_LIMIT)
                {
                    fillableGaps = AOD_TOTAL_LIMIT;
                }
                
                double minCost = MOVE_COST_OFFSET + costPerSubMove(outerDist) + costPerSubMove((fillableGaps - 1) / 2) + 2 * halfStepMoveCost;
                if(fillableGaps > 0 && (!bestMove.has_value() || minCost / fillableGaps < bestCostPerFilledGap))
                {
                    unsigned int minFromLeft = fillableGaps < currentBorderAtomsRight ? 0 : fillableGaps - currentBorderAtomsRight;
                    unsigned int maxFromLeft = currentBorderAtomsLeft < fillableGaps ? currentBorderAtomsLeft : fillableGaps;

                    std::optional<ParallelMove> bestMoveInRow = std::nullopt;
                    unsigned int bestDistInRow = 0;

                    unsigned int *distances = new (std::nothrow) unsigned int[fillableGaps];
                    if(distances == 0)
                    {
                        logger->error("Could not allocate memory");
                        return std::tuple<std::optional<ParallelMove>,int,double>(std::nullopt, 0, __DBL_MAX__);
                    }

                    for(unsigned int atomsFromLeft = minFromLeft; atomsFromLeft <= maxFromLeft; atomsFromLeft++)
                    {
                        ParallelMove::Step start;
                        ParallelMove::Step end;
                        if(rowFirst)
                        {
                            start.colSelection.resize(fillableGaps);
                            start.rowSelection.push_back(iBorder);
                            end.colSelection.resize(fillableGaps);
                            end.rowSelection.push_back(iTarget);
                        }
                        else
                        {
                            start.rowSelection.resize(fillableGaps);
                            start.colSelection.push_back(iBorder);
                            end.rowSelection.resize(fillableGaps);
                            end.colSelection.push_back(iTarget);
                        }

                        auto& outerDimStartVector = rowFirst ? start.colSelection : start.rowSelection;
                        auto& outerDimEndVector = rowFirst ? end.colSelection : end.rowSelection;

                        // Fill distances array first with distances from border atoms to compZone[2]
                        unsigned int positionsFound = 0;
                        size_t currentPosition = innerDimCompZone[0] - 1;
                        for(;currentPosition >= 0 && positionsFound < atomsFromLeft; currentPosition--)
                        {
                            if(accessStateArray(stateArray, iBorder, currentPosition, rowFirst))
                            {
                                positionsFound++;
                                distances[atomsFromLeft - positionsFound] = innerDimCompZone[0] - currentPosition;
                                outerDimStartVector[atomsFromLeft - positionsFound] = (double)currentPosition;
                            }
                        }
                        positionsFound = 0;
                        currentPosition = innerDimCompZone[1];
                        for(;currentPosition < innerSize && positionsFound < fillableGaps - atomsFromLeft; currentPosition++)
                        {
                            if(accessStateArray(stateArray, iBorder, currentPosition, rowFirst))
                            {
                                distances[atomsFromLeft + positionsFound] = currentPosition - (innerDimCompZone[1] - 1);
                                outerDimStartVector[atomsFromLeft + positionsFound] = (double)currentPosition;
                                positionsFound++;
                            }
                        }
                        // Now add distances to gaps (from compZone[2])
                        positionsFound = 0;
                        currentPosition = innerDimCompZone[0];
                        for(;currentPosition < innerDimCompZone[1] && positionsFound < atomsFromLeft; currentPosition++)
                        {
                            if(!accessStateArray(stateArray, iTarget, currentPosition, rowFirst))
                            {
                                distances[positionsFound] += currentPosition - innerDimCompZone[0];
                                outerDimEndVector[positionsFound] = (double)currentPosition;
                                positionsFound++;
                            }
                        }
                        positionsFound = 0;
                        currentPosition = innerDimCompZone[1] - 1;
                        for(;currentPosition >= innerDimCompZone[0] && positionsFound < fillableGaps - atomsFromLeft; currentPosition--)
                        {
                            if(!accessStateArray(stateArray, iTarget, currentPosition, rowFirst))
                            {
                                positionsFound++;
                                distances[fillableGaps - positionsFound] += innerDimCompZone[1] - 1 - currentPosition;
                                outerDimEndVector[atomsFromLeft + positionsFound - 1] = (double)currentPosition;
                            }
                        }

                        unsigned int dist = *std::max_element(distances, distances + fillableGaps);

                        if(!bestMoveInRow.has_value() || dist < bestDistInRow)
                        {
                            bestMoveInRow = ParallelMove::fromStartAndEnd(start, end, logger);
                            bestDistInRow = dist;
                        }
                    }

                    if(bestMoveInRow.has_value())
                    {
                        double costPerGap = bestMoveInRow.value().cost() / (double)fillableGaps;
                        if(!bestMove.has_value() || costPerGap < bestCostPerFilledGap)
                        {
                            bestMove = bestMoveInRow.value();
                            bestFillableGaps = fillableGaps;
                            bestCostPerFilledGap = costPerGap;
                        }
                    }

                    delete[] distances;
                }
            }
        }
        delete[] borderAtomsLeft;
        delete[] borderAtomsRight;
        delete[] emptyCompZoneLocations;
    }

    logger->info("Best direct move can fill {} at a cost of {} per gap", bestFillableGaps, bestCostPerFilledGap);
    return std::tuple<std::optional<ParallelMove>,int,double>(bestMove, bestFillableGaps, bestCostPerFilledGap);
}

std::tuple<std::optional<ParallelMove>,int,double> fillRowSidesDirectly(py::EigenDRef<Eigen::Array<bool, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>& stateArray, 
    size_t compZone[4], std::shared_ptr<spdlog::logger> logger)
{
    std::optional<ParallelMove> bestMove = std::nullopt;
    unsigned int bestFillableGaps = 0;
    double bestCostPerFilledGap = 0;
    for(bool rowFirst : {true, false})
    {
        size_t outerDimCompZone[2];
        size_t innerDimCompZone[2];
        size_t outerSize;
        size_t innerSize;
        unsigned int innerAODLimit = 0;
        unsigned int outerAODLimit = 0;
        fillDimensionDependantData(stateArray, compZone, rowFirst, outerDimCompZone, innerDimCompZone, outerSize, innerSize, outerAODLimit, innerAODLimit);

        for(size_t i = outerDimCompZone[0]; i < outerDimCompZone[1]; i++)
        {
            unsigned int borderAtomsLeft = 0, borderAtomsRight = 0;
            for(size_t j = 0; j < innerDimCompZone[0]; j++)
            {
                if(accessStateArray(stateArray, i, j, rowFirst))
                {
                    borderAtomsLeft++;
                }
            }
            for(size_t j = innerDimCompZone[1]; j < innerSize; j++)
            {
                if(accessStateArray(stateArray, i, j, rowFirst))
                {
                    borderAtomsRight++;
                }
            }

            size_t leftPosition = innerDimCompZone[0], rightPosition = innerDimCompZone[1] - 1;
            unsigned int aodsUsed = 0, fillableGaps = 0;
            while(leftPosition <= rightPosition && (borderAtomsLeft > 0 || borderAtomsRight > 0))
            {
                std::optional<unsigned int> lowestDistToNextGap = std::nullopt;
                bool fromLeft = true;
                size_t newPosition = 0;
                if(borderAtomsLeft > 0)
                {
                    for(size_t j = leftPosition; j <= rightPosition; j++)
                    {
                        if(!accessStateArray(stateArray, i, j, rowFirst))
                        {
                            lowestDistToNextGap = j - leftPosition + 1;
                            newPosition = j + 1;
                            break;
                        }
                    }
                }
                if(borderAtomsRight > 0)
                {
                    for(size_t j = rightPosition; j >= leftPosition; j--)
                    {
                        if(!accessStateArray(stateArray, i, j, rowFirst))
                        {
                            if(!lowestDistToNextGap.has_value() || (rightPosition - j < lowestDistToNextGap.value()))
                            {
                                lowestDistToNextGap = rightPosition - j + 1;
                                fromLeft = false;
                                newPosition = j - 1;
                            }
                            break;
                        }
                    }
                }

                if(!lowestDistToNextGap.has_value() || (aodsUsed + lowestDistToNextGap.value() > AOD_TOTAL_LIMIT) || 
                    (aodsUsed + lowestDistToNextGap.value() > innerAODLimit))
                {
                    break;
                }
                else
                {
                    fillableGaps++;
                    aodsUsed += lowestDistToNextGap.value();
                    if(fromLeft)
                    {
                        leftPosition = newPosition;
                        borderAtomsLeft--;
                    }
                    else
                    {
                        rightPosition = newPosition;
                        borderAtomsRight--;
                    }
                }
            }
            double minCost = MOVE_COST_OFFSET + costPerSubMove((fillableGaps + 1) / 2);
            if(!bestMove.has_value() || (minCost / fillableGaps < bestCostPerFilledGap))
            {
                ParallelMove::Step start;
                ParallelMove::Step end;
                int atomsFromLeft = leftPosition - innerDimCompZone[0];
                int atomsLeftRemaining = atomsFromLeft;
                int atomsFromRight = innerDimCompZone[1] - 1 - rightPosition;
                int atomsRightRemaining = atomsFromRight;

                if(rowFirst)
                {
                    start.colSelection.resize(atomsFromLeft + atomsFromRight);
                    start.rowSelection.push_back(i);
                    end.colSelection.resize(atomsFromLeft + atomsFromRight);
                    end.rowSelection.push_back(i);
                }
                else
                {
                    start.rowSelection.resize(atomsFromLeft + atomsFromRight);
                    start.colSelection.push_back(i);
                    end.rowSelection.resize(atomsFromLeft + atomsFromRight);
                    end.colSelection.push_back(i);
                }

                auto& outerDimStartVector = rowFirst ? start.colSelection : start.rowSelection;
                auto& outerDimEndVector = rowFirst ? end.colSelection : end.rowSelection;

                size_t j = leftPosition - 1;
                for(; j >= 0 && atomsLeftRemaining > 0; j--)
                {
                    if(j >= innerDimCompZone[0])
                    {
                        outerDimEndVector[j - innerDimCompZone[0]] = j;
                    }
                    if(accessStateArray(stateArray, i, j, rowFirst))
                    {
                        atomsLeftRemaining--;
                        outerDimStartVector[atomsLeftRemaining] = j;
                    }
                }
                int distanceLeft = (int)innerDimCompZone[0] - (int)j;
                j = rightPosition + 1;
                for(; j < innerSize && atomsRightRemaining > 0; j++)
                {
                    if(j < innerDimCompZone[1])
                    {
                        outerDimEndVector[atomsFromLeft + atomsFromRight - (innerDimCompZone[1] - j)] = j;
                    }
                    if(accessStateArray(stateArray, i, j, rowFirst))
                    {
                        atomsRightRemaining--;
                        outerDimStartVector[atomsFromLeft + atomsFromRight - atomsRightRemaining - 1] = j;
                    }
                }
                int distanceRight = (int)j - (int)(innerDimCompZone[1] - 1);
                if(atomsLeftRemaining > 0 || atomsRightRemaining > 0 || distanceLeft < 0 || distanceRight < 0)
                {
                    logger->error("This point in fillRowSidesDirectly should never be reached!");
                    continue;
                }
                
                ParallelMove move;
                move.steps.push_back(std::move(start));
                move.steps.push_back(std::move(end));
                double costPerGap = move.cost() / (double)fillableGaps;
                if(!bestMove.has_value() || costPerGap < bestCostPerFilledGap)
                {
                    bestMove = move;
                    bestFillableGaps = fillableGaps;
                    bestCostPerFilledGap = costPerGap;
                }
            }
        }
    }

    logger->info("Best direct move can fill {} at a cost of {} per gap", bestFillableGaps, bestCostPerFilledGap);
    return std::tuple<std::optional<ParallelMove>,int,double>(bestMove, bestFillableGaps, bestCostPerFilledGap);
}

bool analyzeArray(py::EigenDRef<Eigen::Array<bool, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>& stateArray, 
    size_t compZone[4], unsigned int& vacancies, std::shared_ptr<spdlog::logger> logger)
{
    vacancies = 0;
    for(size_t row = 0; row < (size_t)stateArray.rows(); row++)
    {
        for(size_t col = 0; col < (size_t)stateArray.cols(); col++)
        {
            if(!stateArray(row,col) && row >= compZone[0] && row < compZone[1] && col >= compZone[2] && col < compZone[3])
            {
                vacancies++;
            }
        }
    }
    std::stringstream arrayString;
    arrayString << stateArray;
    logger->debug(arrayString.str());
    logger->debug("{} vacancies (analyzeArray)", vacancies);
    return true;
}

bool findNextMove(py::EigenDRef<Eigen::Array<bool, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>& stateArray, 
    size_t compZone[4], std::vector<ParallelMove>& moves, bool& sorted, unsigned int& vacancies, std::shared_ptr<spdlog::logger> logger)
{
    logger->debug("Finding next best move");
    std::optional<ParallelMove> bestMove = std::nullopt;
    double bestCostPerFilledSites = 0;
    int bestFilledSites = 0;
    for(const auto& function : {fillRowSidesDirectly, fillRowThroughSubspace, fillColumnHorizontally})
    {
        auto [move, filledSites, costPerFilledGap] = function(stateArray, compZone, logger);
        if(move.has_value() && filledSites > 0)
        {
            if(!bestMove.has_value() || costPerFilledGap < bestCostPerFilledSites)
            {
                bestCostPerFilledSites = costPerFilledGap;
                bestFilledSites = filledSites;
                bestMove = move.value();
            }
        }
    }
    vacancies -= bestFilledSites;
    if(!bestMove.has_value())
    {
        logger->error("Finding next best move");
        return false;
    }
    else
    {
        if(!bestMove.value().execute(stateArray, logger))
        {
            logger->error("Error when executing move");
            return false;
        }
        else
        {
            moves.push_back(std::move(bestMove.value()));
        }
    }
    logger->debug("{} vacancies", vacancies);
    if(vacancies == 0)
    {
        sorted = true;
        logger->debug("Sorting done");
    }
    return true;
}

std::optional<std::vector<ParallelMove>> sortParallel(
    py::EigenDRef<Eigen::Array<bool, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>& stateArray, 
    size_t compZoneRowStart, size_t compZoneRowEnd, size_t compZoneColStart, size_t compZoneColEnd)
{
    std::shared_ptr<spdlog::logger> logger;
    Config& config = Config::getInstance();
    if((logger = spdlog::get(config.parallelLoggerName)) == nullptr)
    {
        logger = spdlog::basic_logger_mt(config.parallelLoggerName, config.logFileName);
    }
    logger->set_level(spdlog::level::debug);

    size_t compZone[4] = {compZoneRowStart, compZoneRowEnd, compZoneColStart, compZoneColEnd};
    std::vector<ParallelMove> moves;
    if(compZoneRowStart >= compZoneRowEnd || compZoneRowEnd > (size_t)stateArray.rows() || 
        compZoneColStart >= compZoneColEnd || compZoneColEnd > (size_t)stateArray.cols())
    {
        logger->error("No suitable arguments");
        return std::nullopt;
    }
    unsigned int vacancies = 0;
    analyzeArray(stateArray, compZone, vacancies, logger);
    bool sorted = false;
    while(!sorted)
    {
        if(!findNextMove(stateArray, compZone, moves, sorted, vacancies, logger))
        {
            logger->error("No move could be found");
            return std::nullopt;
        }
    }
    return moves;
}
