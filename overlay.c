#include <windows.h>
#include <GL/gl.h>

void BeginOverlay(int Width, int Height)
{
	glPushAttrib(GL_ENABLE_BIT|GL_VIEWPORT_BIT);

	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);

	glViewport(0, 0, Width, Height);
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
	glOrtho(0.0f, Width, 0.0f, Height, 0.0f, 1.0f);
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glLoadIdentity();
}

void EndOverlay(void)
{
	glPopMatrix();
	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
	glMatrixMode(GL_MODELVIEW);

	glDepthMask(GL_TRUE);
	glEnable(GL_DEPTH_TEST);

	glPopAttrib();
}
