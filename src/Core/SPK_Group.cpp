//////////////////////////////////////////////////////////////////////////////////
// SPARK particle engine														//
// Copyright (C) 2008-2009 - Julien Fryer - julienfryer@gmail.com				//
//																				//
// This software is provided 'as-is', without any express or implied			//
// warranty.  In no event will the authors be held liable for any damages		//
// arising from the use of this software.										//
//																				//
// Permission is granted to anyone to use this software for any purpose,		//
// including commercial applications, and to alter it and redistribute it		//
// freely, subject to the following restrictions:								//
//																				//
// 1. The origin of this software must not be misrepresented; you must not		//
//    claim that you wrote the original software. If you use this software		//
//    in a product, an acknowledgment in the product documentation would be		//
//    appreciated but is not required.											//
// 2. Altered source versions must be plainly marked as such, and must not be	//
//    misrepresented as being the original software.							//
// 3. This notice may not be removed or altered from any source distribution.	//
//////////////////////////////////////////////////////////////////////////////////


#include "Core/SPK_Group.h"
#include "Core/SPK_Emitter.h"
#include "Core/SPK_Modifier.h"
#include "Core/SPK_Renderer.h"
#include "Core/SPK_Factory.h"
#include "Core/SPK_Buffer.h"


namespace SPK
{
	bool Group::bufferManagement = true;

	Group::Group(Model* model,size_t capacity) :
		Registerable(),
		model(model != NULL ? model : &defaultModel),
		renderer(NULL),
		friction(0.0f),
		gravity(Vector3D()),
		pool(Pool<Particle>(capacity)),
		particleData(new Particle::ParticleData[capacity]),
		particleEnableParams(new float[capacity * model->getNbValuesInParticleEnableArray()]),
		particleMutableParams(new float[capacity * model->getNbValuesInParticleMutableArray()]),
		sortingEnabled(false),
		distanceComputationEnabled(false),
		creationBuffer(),
		nbBufferedParticles(0),
		fupdate(NULL),
		fbirth(NULL),
		fdeath(NULL),
		boundingBoxEnabled(false),
		emitters(),
		modifiers(),
		activeModifiers(),
		additionalBuffers(),
		emitterRemoval(false)
	{}

	Group::Group(const Group& group) :
		Registerable(group),
		model(group.model),
		renderer(group.renderer),
		friction(group.friction),
		gravity(group.gravity),
		pool(group.pool),
		sortingEnabled(group.sortingEnabled),
		distanceComputationEnabled(group.distanceComputationEnabled),
		creationBuffer(group.creationBuffer),
		nbBufferedParticles(group.nbBufferedParticles),
		fupdate(group.fupdate),
		fbirth(group.fbirth),
		fdeath(group.fdeath),
		emitters(group.emitters),
		modifiers(group.modifiers),
		activeModifiers(group.activeModifiers.capacity()),
		additionalBuffers(group.additionalBuffers),
		emitterRemoval(group.emitterRemoval)
	{
		particleData = new Particle::ParticleData[pool.getNbReserved()];
		particleEnableParams = new float[pool.getNbReserved() * model->getNbValuesInParticleEnableArray()];
		particleMutableParams = new float[pool.getNbReserved() * model->getNbValuesInParticleMutableArray()];
		
		memcpy(particleData,group.particleData,pool.getNbTotal() * sizeof(Particle::ParticleData));
		memcpy(particleEnableParams,group.particleEnableParams,pool.getNbTotal() * sizeof(float) * model->getNbValuesInParticleEnableArray());
		memcpy(particleMutableParams,group.particleMutableParams,pool.getNbTotal() * sizeof(float) * model->getNbValuesInParticleMutableArray());

		for (Pool<Particle>::iterator it = pool.begin(); it != pool.endInactive(); ++it)
		{
			it->group = this;
			it->data = particleData + it->index;
			it->enableParams = particleEnableParams + it->index * model->getNbValuesInParticleEnableArray();
			it->mutableParams = particleMutableParams + it->index * model->getNbValuesInParticleMutableArray();
		}

		// copy additional buffers
		for (std::map<std::string,Buffer*>::iterator it = additionalBuffers.begin(); it != additionalBuffers.end(); ++it)
		{
			Buffer* buffer = it->second;
			it->second = buffer->clone();
		}
	}

	Group::~Group()
	{
		delete[] particleData;
		delete[] particleEnableParams;
		delete[] particleMutableParams;

		// destroys additional buffers
		destroyAllBuffers();
	}

	void Group::registerChildren(bool registerAll)
	{
		Registerable::registerChildren(registerAll);
		
		registerChild(model,registerAll);
		registerChild(renderer,registerAll);
		
		for (std::vector<Emitter*>::const_iterator it = emitters.begin(); it != emitters.end(); ++it)
			registerChild(*it,registerAll);
		for (std::vector<Modifier*>::const_iterator it = modifiers.begin(); it != modifiers.end(); ++it)
			registerChild(*it,registerAll);
	}

	void Group::copyChildren(const Group& group,bool createBase)
	{
		Registerable::copyChildren(group,createBase);

		model = dynamic_cast<Model*>(copyChild(group.model,createBase));
		renderer = dynamic_cast<Renderer*>(copyChild(group.renderer,createBase));

		// we clear the copies of pointers pushed in the vectors by the copy constructor
		emitters.clear();
		modifiers.clear();

		for (std::vector<Emitter*>::const_iterator it = group.emitters.begin(); it != group.emitters.end(); ++it)
			emitters.push_back(dynamic_cast<Emitter*>(copyChild(*it,createBase)));
		for (std::vector<Modifier*>::const_iterator it = group.modifiers.begin(); it != group.modifiers.end(); ++it)
			modifiers.push_back(dynamic_cast<Modifier*>(copyChild(*it,createBase)));
	}

	void Group::destroyChildren(bool keepChildren)
	{
		destroyChild(model,keepChildren);
		destroyChild(renderer,keepChildren);

		for (std::vector<Emitter*>::const_iterator it = emitters.begin(); it != emitters.end(); ++it)
			destroyChild(*it,keepChildren);
		for (std::vector<Modifier*>::const_iterator it = modifiers.begin(); it != modifiers.end(); ++it)
			destroyChild(*it,keepChildren);

		Registerable::destroyChildren(keepChildren);
	}

	void Group::setRenderer(Renderer* renderer)
	{
		decrementChildReference(this->renderer);
		incrementChildReference(renderer);

		if ((bufferManagement)&&(renderer != this->renderer))
		{
			if (this->renderer != NULL) this->renderer->destroyBuffers(*this);
			if (renderer != NULL) renderer->createBuffers(*this);
		}

		this->renderer = renderer;
	}

	void Group::addEmitter(Emitter* emitter)
	{
		if (emitter == NULL)
			return;

		// Checks if the emitter is already in the group (since 1.03.03)
		std::vector<Emitter*>::const_iterator it = std::find(emitters.begin(),emitters.end(),emitter);
		if (it != emitters.end())
			return;

		incrementChildReference(emitter);
		emitters.push_back(emitter);
	}

	void Group::removeEmitter(Emitter* emitter)
	{
		std::vector<Emitter*>::iterator it = std::find(emitters.begin(),emitters.end(),emitter);
		if (it != emitters.end())
		{
			decrementChildReference(emitter);
			emitters.erase(it);
		}
	}

	void Group::addModifier(Modifier* modifier)
	{
		if (modifier == NULL)
			return;

		incrementChildReference(modifier);

		if (bufferManagement)
			modifier->createBuffers(*this);

		modifiers.push_back(modifier);
	}

	void Group::removeModifier(Modifier* modifier)
	{
		std::vector<Modifier*>::iterator it = std::find(modifiers.begin(),modifiers.end(),modifier);
		if (it != modifiers.end())
		{
			decrementChildReference(modifier);

			if (bufferManagement)
				(*it)->createBuffers(*this);

			modifiers.erase(it);
		}
	}

	bool Group::update(float deltaTime)
	{
		unsigned int nbManualBorn = nbBufferedParticles;
		unsigned int nbAutoBorn = 0;

		bool hasActiveEmitters = false;
		bool hasSleepingEmitters = false;
		std::vector<Emitter*>::const_iterator endIt = emitters.end();

		// Updates emitters
		std::vector<Emitter*>::iterator emitterIt = emitters.begin();
		for (std::vector<Emitter*>::const_iterator it = emitters.begin(); it != endIt; ++it)
		{
			int nb = (*it)->updateNumber(deltaTime);
			if ((nb == 0)&&(it == emitterIt))
				++emitterIt;
			nbAutoBorn += nb;
			hasActiveEmitters |= !((*it)->isSleeping());
			hasSleepingEmitters |= (*it)->isSleeping();
		}

		unsigned int nbBorn = nbAutoBorn + nbManualBorn;

		// Inits bounding box
		if (boundingBoxEnabled)
		{
			const float maxFloat = std::numeric_limits<float>::max();
			AABBMin.set(maxFloat,maxFloat,maxFloat);
			AABBMax.set(-maxFloat,-maxFloat,-maxFloat);
		}

		// Prepare modifiers for processing
		activeModifiers.clear();
		for (std::vector<Modifier*>::iterator it = modifiers.begin(); it != modifiers.end(); ++it)
		{
			(*it)->beginProcess(*this);
			if ((*it)->isActive())
				activeModifiers.push_back(*it);
		}

		// Updates particles
		for (size_t i = 0; i < pool.getNbActive(); ++i)
		{
			if ((pool[i].update(deltaTime))||((fupdate != NULL)&&((*fupdate)(pool[i],deltaTime))))
			{
				if (fdeath != NULL)
					(*fdeath)(pool[i]);

				if (nbBorn > 0)
				{
					pool[i].init();
					launchParticle(pool[i],emitterIt,nbManualBorn);
					--nbBorn;
				}
				else
				{
					particleData[i].sqrDist = 0.0f;
					pool.makeInactive(i);
					--i;
				}
			}
			else
			{
				if (boundingBoxEnabled)
					updateAABB(pool[i]);

				if (distanceComputationEnabled)
					pool[i].computeSqrDist();
			}
		}

		// Terminates modifiers processing
		for (std::vector<Modifier*>::iterator it = modifiers.begin(); it != modifiers.end(); ++it)
			(*it)->endProcess(*this);

		// Emits new particles if some left
		for (int i = nbBorn; i > 0; --i)
			pushParticle(emitterIt,nbManualBorn);

		// Sorts particles if enabled
		if ((sortingEnabled)&&(pool.getNbActive() > 1))
			sortParticles(0,pool.getNbActive() - 1);

		if ((!boundingBoxEnabled)||(pool.getNbActive() == 0))
		{
			AABBMin.set(0.0f,0.0f,0.0f);
			AABBMax.set(0.0f,0.0f,0.0f);
		}

		// Removes sleeping emitters
		if ((hasSleepingEmitters)&&(emitterRemoval)&&(isRegistered()))
		{
			std::vector<Emitter*>::iterator it = emitters.begin();
			while(it != emitters.end())
				if (((*it)->isSleeping())&&((*it)->isRegistered())&&((*it)->isDestroyable()))
				{
					decrementChildReference(*it);
					SPKFactory::getInstance().destroy((*it)->getID());
					it = emitters.erase(it);
				}
				else ++it;
		}

		return (hasActiveEmitters)||(pool.getNbActive() > 0);
	}

	void Group::pushParticle(std::vector<Emitter*>::iterator& emitterIt,unsigned int& nbManualBorn)
	{
		Particle* ptr = pool.makeActive();
		if (ptr == NULL)
		{
			if (pool.getNbEmpty() > 0)
			{
				Particle p(this,pool.getNbActive());
				launchParticle(p,emitterIt,nbManualBorn);
				pool.pushActive(p);
			}
			else if (nbManualBorn > 0)
				popNextManualAdding(nbManualBorn);
		}
		else
		{
			ptr->init();
			launchParticle(*ptr,emitterIt,nbManualBorn);
		}
	}

	void Group::launchParticle(Particle& p,std::vector<Emitter*>::iterator& emitterIt,unsigned int& nbManualBorn)
	{
		if (nbManualBorn == 0)
		{
			if ((*emitterIt)->launchParticle(p) <= 0)
			{
				++emitterIt;
				std::vector<Emitter*>::const_iterator lastEmitterIt = emitters.end() - 1;
				while((emitterIt < lastEmitterIt)&&((*emitterIt)->nbBorn <= 0))
					++emitterIt;
			}
		}
		else
		{
			CreationData creationData = creationBuffer.front();

			if (creationData.zone != NULL)
				creationData.zone->generatePosition(p,creationData.full);
			else
				p.position() = creationData.position;

			if (creationData.emitter != NULL)
				creationData.emitter->generateVelocity(p);
			else
				p.velocity() = creationData.velocity;

			popNextManualAdding(nbManualBorn);
		}

		// Resets old position (fix 1.04.00)
		p.oldPosition() = p.position();

		if (fbirth != NULL)
			(*fbirth)(p);

		if (boundingBoxEnabled)
			updateAABB(p);

		if (distanceComputationEnabled)
			p.computeSqrDist();
	}

	void Group::render()
	{
		if ((renderer == NULL)||(!renderer->isActive()))
			return;

		renderer->render(*this);
	}

	void Group::empty()
	{
		for (size_t i = 0; i < pool.getNbActive(); ++i)
			particleData[i].sqrDist = 0.0f;

		pool.makeAllInactive();
		creationBuffer.clear();
		nbBufferedParticles = 0;
	}

	void Group::flushAddedParticles()
	{
		unsigned int nbManualBorn = nbBufferedParticles;
		std::vector<Emitter*>::iterator emitterIt;
		while(nbManualBorn > 0)
			pushParticle(emitterIt,nbManualBorn);
	}

	float Group::addParticles(const Vector3D& start,const Vector3D& end,const Emitter* emitter,float step,float offset)
	{
		if ((step <= 0.0f)||(offset < 0.0f))
			return 0.0f;

		Vector3D displacement = end - start;
		float totalDist = displacement.getNorm();

		while(offset < totalDist)
		{
			Vector3D position = start;
			position += displacement * offset / totalDist;
			addParticles(1,position,Vector3D(),NULL,emitter);
			offset += step;
		}

		return offset - totalDist;
	}

	float Group::addParticles(const Vector3D& start,const Vector3D& end,const Vector3D& velocity,float step,float offset)
	{
		if ((step <= 0.0f)||(offset < 0.0f))
			return 0.0f;

		Vector3D displacement = end - start;
		float totalDist = displacement.getNorm();

		while(offset < totalDist)
		{
			Vector3D position = start;
			position += displacement * (offset / totalDist);
			addParticles(1,position,velocity,NULL,NULL);
			offset += step;
		}

		return offset - totalDist;
	}

	void Group::addParticles(unsigned int nb,const Vector3D& position,const Vector3D& velocity,const Zone* zone,const Emitter* emitter,bool full)
	{
		if (nb == 0)
			return;

		CreationData data = {nb,position,velocity,zone,emitter,full};
		creationBuffer.push_back(data);
		nbBufferedParticles += nb;
	}

	void Group::addParticles(unsigned int nb,const Emitter* emitter)
	{
		addParticles(nb,Vector3D(),Vector3D(),emitter->getZone(),emitter,emitter->isFullZone());
	}

	void Group::addParticles(const Zone* zone,const Emitter* emitter,float deltaTime,bool full)
	{
		addParticles(emitter->updateNumber(deltaTime),Vector3D(),Vector3D(),zone,emitter,full);
	}

	void Group::addParticles(const Vector3D& position,const Emitter* emitter,float deltaTime)
	{
		addParticles(emitter->updateNumber(deltaTime),position,Vector3D(),NULL,emitter);
	}

	void Group::addParticles(const Emitter* emitter,float deltaTime)
	{
		addParticles(emitter->updateNumber(deltaTime),Vector3D(),Vector3D(),emitter->getZone(),emitter,emitter->isFullZone());
	}

	void Group::sortParticles()
	{
		computeDistances();

		if (sortingEnabled)
			sortParticles(0,pool.getNbActive() - 1);
	}

	void Group::computeDistances()
	{
		if (!distanceComputationEnabled)
			return;

		Pool<Particle>::const_iterator endIt = pool.end();
		for (Pool<Particle>::iterator it = pool.begin(); it != endIt; ++it)
			it->computeSqrDist();
	}

	void Group::computeAABB()
	{
		if ((!boundingBoxEnabled)||(pool.getNbActive() == 0))
		{
			AABBMin.set(0.0f,0.0f,0.0f);
			AABBMax.set(0.0f,0.0f,0.0f);
			return;
		}

		const float maxFloat = std::numeric_limits<float>::max();
		AABBMin.set(maxFloat,maxFloat,maxFloat);
		AABBMax.set(-maxFloat,-maxFloat,-maxFloat);

		Pool<Particle>::iterator endIt = pool.end();
		for (Pool<Particle>::iterator it = pool.begin(); it != endIt; ++it)
			updateAABB(*it);
	}

	void Group::reallocate(size_t capacity)
	{
		if (capacity > pool.getNbReserved())
		{
			pool.reallocate(capacity);

			Particle::ParticleData* newData = new Particle::ParticleData[pool.getNbReserved()];
			float* newEnableParams = new float[pool.getNbReserved() * model->getNbValuesInParticleEnableArray()];
			float* newMutableParams = new float[pool.getNbReserved() * model->getNbValuesInParticleMutableArray()];

			memcpy(newData,particleData,pool.getNbTotal() * sizeof(Particle::ParticleData));
			memcpy(newEnableParams,particleEnableParams,pool.getNbTotal() * sizeof(float) * model->getNbValuesInParticleEnableArray());
			memcpy(newMutableParams,particleMutableParams,pool.getNbTotal() * sizeof(float) * model->getNbValuesInParticleMutableArray());

			delete[] particleData;
			delete[] particleEnableParams;
			delete[] particleMutableParams;

			particleData = newData;
			particleEnableParams = newEnableParams;
			particleMutableParams = newMutableParams;

			// Destroys all the buffers
			destroyAllBuffers();
		}
	}

	void Group::popNextManualAdding(unsigned int& nbManualBorn)
	{
		--creationBuffer.front().nb;
		--nbManualBorn;
		--nbBufferedParticles;
		if (creationBuffer.front().nb <= 0)
			creationBuffer.pop_front();
	}

	void Group::updateAABB(const Particle& particle)
	{
		const Vector3D& position = particle.position();
		if (AABBMin.x > position.x)
			AABBMin.x = position.x;
		if (AABBMin.y > position.y)
			AABBMin.y = position.y;
		if (AABBMin.z > position.z)
			AABBMin.z = position.z;
		if (AABBMax.x < position.x)
			AABBMax.x = position.x;
		if (AABBMax.y < position.y)
			AABBMax.y = position.y;
		if (AABBMax.z < position.z)
			AABBMax.z = position.z;
	}

	const void* Group::getParamAddress(ModelParam param) const
	{
		return particleEnableParams + model->getParameterEnableOffset(param);
	}

	size_t Group::getParamStride() const
	{
		return model->getNbValuesInParticleEnableArray() * sizeof(float);
	}

	Buffer* Group::createBuffer(const std::string& ID,const BufferCreator& creator,unsigned int flag,bool swapEnabled) const
	{
		destroyBuffer(ID);

		Buffer* buffer = creator.createBuffer(pool.getNbReserved(),*this);

		buffer->flag = flag;
		buffer->swapEnabled = swapEnabled;

		additionalBuffers.insert(std::pair<std::string,Buffer*>(ID,buffer));

		return buffer;
	}

	void Group::destroyBuffer(const std::string& ID) const
	{
		std::map<std::string,Buffer*>::iterator it = additionalBuffers.find(ID);

		if (it != additionalBuffers.end())
		{
			delete it->second;
			additionalBuffers.erase(it);
		}
	}

	void Group::destroyAllBuffers() const
	{
		for (std::map<std::string,Buffer*>::const_iterator it = additionalBuffers.begin(); it != additionalBuffers.end(); ++it)
			delete it->second;
		additionalBuffers.clear();
	}

	Buffer* Group::getBuffer(const std::string& ID,unsigned int flag) const
	{
		Buffer* buffer = getBuffer(ID);

		if ((buffer != NULL)&&(buffer->flag == flag))
			return buffer;

		return NULL;
	}

	Buffer* Group::getBuffer(const std::string& ID) const
	{
		std::map<std::string,Buffer*>::const_iterator it = additionalBuffers.find(ID);

		if (it != additionalBuffers.end())
			return it->second;

		return NULL;
	}

	void Group::enableBuffersManagement(bool manage)
	{
		bufferManagement = manage;
	}

	bool Group::isBuffersManagementEnabled()
	{
		return bufferManagement;
	}

	void Group::sortParticles(int start,int end)
	{
		if (start < end)
		{
			int i = start - 1;
			int j = end + 1;
			float pivot = particleData[(start + end) >> 1].sqrDist;
			while (true)
			{
				do ++i;
				while (particleData[i].sqrDist > pivot);
				do --j;	
				while (particleData[j].sqrDist < pivot);
				if (i < j)
					swapParticles(pool[i],pool[j]);
				else break;
			}

			sortParticles(start,j);
			sortParticles(j + 1,end);
		}
	}
}