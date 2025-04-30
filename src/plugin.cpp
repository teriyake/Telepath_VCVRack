#include "plugin.hpp"

Plugin* pluginInstance;

void init(Plugin* p)
{
	pluginInstance = p;

	p->addModel(modelOSCRX);
	p->addModel(modelOSCTX);
	p->addModel(modelATX);
}
