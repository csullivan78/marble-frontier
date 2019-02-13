function [path] = frontierPlan(occGrid, position, hblob, minObsDist, figNum)
%   (occupancy grid, agent position, blob detector, minimum obstacle distance, figure number)
    %% Create Reachability Grid
    speedGrid = bwdist(occGrid);
    satSpeedGrid = speedGrid;
    if nargin == 4
        satSpeedGrid(satSpeedGrid >= minObsDist) = minObsDist;
    end
    satSpeedGrid(satSpeedGrid == 0) = 1e-6;
    reachGrid = msfm(double(satSpeedGrid), [position(2); position(1)]);

    %%  Find frontier grid cells (Open cells adjacent to unexplored cells)
    frontGrid = findFrontier(occGrid);
    if nargin >= 3
        [area, centroids, bbox, labels] = step(hblob, logical(frontGrid));
    end
    frontGrid = double(labels >= 1);
    if sum(frontGrid(:)) <= 10
        path = [];
        return
    end
  
    frontCost = frontGrid.*reachGrid + (1 - frontGrid)*1e6;
    [~, idNext] = min(frontCost(:));
    [i_goal, j_goal] = ind2sub(size(occGrid), idNext);
    path = findPathContinuous(reachGrid, [i_goal, j_goal], [position(1), position(2)]);
    
    if nargin == 5
        figure(figNum)
        subplot(2,2,1);
        h = pcolor(satSpeedGrid);
        set(h, 'EdgeColor', 'none');
        title('Saturated ESDF')
        ax2 = subplot(2,2,2);
        maxReachFree = 50;
        reachGrid(reachGrid >= maxReachFree) = maxReachFree;
%         h = pcolor(reachGrid);
%         hold on
        h = contourf(log(reachGrid), 'LevelStep', 0.5);
        colormap(ax2, cool)
%         set(h, 'EdgeColor', 'none');
        title('Reachability (Cost)')
        subplot(2,2,3)
        h = pcolor(frontGrid);
        set(h, 'EdgeColor', 'none');
        hold on
        for i = 1:size(bbox,1)
            plot(centroids(i,1), centroids(i,2));
            rectangle('Position', bbox(i,:));
        end
        hold off
        title('Frontier')
        subplot(2,2,4)
        h = pcolor(occGrid);
        set(h, 'EdgeColor', 'none');
        hold on
        plot(path(:,1), path(:,2), 'r');
        plot(position(1), position(2), 'r*');
        title('Path')
        hold off
        set(gcf, 'Position', [1, 1, 1080, 1080]);
        axis equal
        axis tight
        tightfig;
    end
    
end

