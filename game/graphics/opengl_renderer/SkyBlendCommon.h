#pragma once

#include <GL/glew.h>
#include <GLFW/glfw3.h>

struct SkyBlendStats {
  int sky_draws = 0;
  int cloud_draws = 0;
  int sky_blends = 0;
  int cloud_blends = 0;
};

class SkyBlendCommon {
public:
    SkyBlendCommon();
    ~SkyBlendCommon();

    void initOpenGL();
    void render();

private:
    GLFWwindow* window;
};

class SkyBlendCommon {
public:
    SkyBlendCommon();
    ~SkyBlendCommon();

    void initOpenGL();
    void render();

private:
    GLFWwindow* window;
};
