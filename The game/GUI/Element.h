#pragma once

class RenderMaster;
class Model2D;
namespace core::gl {
	class Texture;
}

class Element {
public:
	virtual void draw(RenderMaster &renderer) = 0;
	virtual Model2D& getModel() = 0;
	virtual core::gl::Texture* getTexture() = 0;
};