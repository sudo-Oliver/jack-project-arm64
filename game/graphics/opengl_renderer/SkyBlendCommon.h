#pragma once

#include <GL/glew.h>
#include <GLFW/glfw3.h>

struct SkyBlendStats {
  int sky_draws;
  int cloud_draws;
  int sky_blends;
  int cloud_blends;

  SkyBlendStats() : sky_draws(0), cloud_draws(0), sky_blends(0), cloud_blends(0) {}
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
