dims = csvread('reach.csv', 0, 0, [0 0 0 3]);
reach = csvread('reach.csv', 1, 0);
reach = reshape(reach, dims(1:3));
reach(reach >= 20) = 20;
[X, Y, Z] = meshgrid(1:dims(2), 1:dims(1), 1:dims(3));
% scatter3(X(:), Y(:), Z(:), 10, reach(:));
% surf(reach(:,:,4));
% view(2);
contourf(reach(:,:,6), 'LevelStep', 0.5);
colormap(cool);