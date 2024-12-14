#pragma once
#include <gl\glew.h>

class FBO {
public:
	FBO(int w, int h); // w - width, h - height (of screen)
	~FBO();

	void clear();

	void bindBuffer();
	void bindTexture();
	void unbind() const;

private:
	GLuint m_fbo = 0;
	GLuint m_rbo = 0;
	GLuint m_text = 0;
};