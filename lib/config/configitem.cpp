/******************************************************************************
 * Icinga 2                                                                   *
 * Copyright (C) 2012 Icinga Development Team (http://www.icinga.org/)        *
 *                                                                            *
 * This program is free software; you can redistribute it and/or              *
 * modify it under the terms of the GNU General Public License                *
 * as published by the Free Software Foundation; either version 2             *
 * of the License, or (at your option) any later version.                     *
 *                                                                            *
 * This program is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with this program; if not, write to the Free Software Foundation     *
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.             *
 ******************************************************************************/

#include "i2-config.h"

using namespace icinga;

recursive_mutex ConfigItem::m_Mutex;
ConfigItem::ItemMap ConfigItem::m_Items;
signals2::signal<void (const ConfigItem::Ptr&)> ConfigItem::OnCommitted;
signals2::signal<void (const ConfigItem::Ptr&)> ConfigItem::OnRemoved;

/**
 * Constructor for the ConfigItem class.
 *
 * @param type The object type.
 * @param name The name of the item.
 * @param exprl Expression list for the item.
 * @param parents Parent objects for the item.
 * @param debuginfo Debug information.
 */
ConfigItem::ConfigItem(const String& type, const String& name,
    const String& unit, const ExpressionList::Ptr& exprl,
    const vector<String>& parents, const DebugInfo& debuginfo)
	: m_Type(type), m_Name(name), m_Unit(unit), m_ExpressionList(exprl),
	  m_Parents(parents), m_DebugInfo(debuginfo)
{
}

/**
 * Retrieves the type of the configuration item.
 *
 * @returns The type.
 */
String ConfigItem::GetType(void) const
{
	return m_Type;
}

/**
 * Retrieves the name of the configuration item.
 *
 * @returns The name.
 */
String ConfigItem::GetName(void) const
{
	return m_Name;
}

/**
 * Retrieves the name of the compilation unit this item belongs to.
 *
 * @returns The unit name.
 */
String ConfigItem::GetUnit(void) const
{
	return m_Unit;
}

/**
 * Retrieves the debug information for the configuration item.
 *
 * @returns The debug information.
 */
DebugInfo ConfigItem::GetDebugInfo(void) const
{
	return m_DebugInfo;
}

/**
 * Retrieves the expression list for the configuration item.
 *
 * @returns The expression list.
 */
ExpressionList::Ptr ConfigItem::GetExpressionList(void) const
{
	return m_ExpressionList;
}

/**
 * Retrieves the list of parents for the configuration item.
 *
 * @returns The list of parents.
 */
vector<String> ConfigItem::GetParents(void) const
{
	return m_Parents;
}

set<ConfigItem::WeakPtr> ConfigItem::GetChildren(void) const
{
	return m_ChildObjects;
}

Dictionary::Ptr ConfigItem::Link(void) const
{
	Dictionary::Ptr attrs = boost::make_shared<Dictionary>();
	InternalLink(attrs);
	return attrs;
}

/**
 * Calculates the object's properties based on parent objects and the object's
 * expression list.
 *
 * @param dictionary The dictionary that should be used to store the
 *		     properties.
 */
void ConfigItem::InternalLink(const Dictionary::Ptr& dictionary) const
{
	BOOST_FOREACH(const String& name, m_Parents) {
		ConfigItem::Ptr parent;

		ConfigCompilerContext *context = ConfigCompilerContext::GetContext();

		if (context)
			parent = context->GetItem(GetType(), name);

		/* ignore already active objects while we're in the compiler
		 * context and linking to existing items is disabled. */
		if (!parent && (!context || (context->GetFlags() & CompilerLinkExisting)))
			parent = ConfigItem::GetObject(GetType(), name);

		if (!parent) {
			stringstream message;
			message << "Parent object '" << name << "' does not"
			    " exist (" << m_DebugInfo << ")";
			BOOST_THROW_EXCEPTION(domain_error(message.str()));
		}

		parent->InternalLink(dictionary);
	}

	m_ExpressionList->Execute(dictionary);
}

/**
 * Commits the configuration item by creating or updating a DynamicObject
 * object.
 *
 * @returns The DynamicObject that was created/updated.
 */
DynamicObject::Ptr ConfigItem::Commit(const ConfigItem::Ptr& self)
{
	String type, name;
	DynamicObject::Ptr dobj;
	vector<String> parents;
	Dictionary::Ptr properties;

	{
		ObjectLock olock(self);
		type = self->GetType();
		name = self->GetName();
		dobj = self->GetDynamicObject();
		parents = self->GetParents();
		properties = self->Link();
	}

	Logger::Write(LogDebug, "base", "Commit called for ConfigItem Type=" + type + ", Name=" + name);

	/* Make sure the type is valid. */
	DynamicType::Ptr dtype = DynamicType::GetByName(type);

	if (!dtype)
		BOOST_THROW_EXCEPTION(runtime_error("Type '" + type + "' does not exist."));

	{
		recursive_mutex::scoped_lock lock(m_Mutex);

		/* Try to find an existing item with the same type and name. */
		pair<String, String> ikey = make_pair(type, name);
		ItemMap::iterator it = m_Items.find(ikey);

		set<ConfigItem::WeakPtr> children;

		if (it != m_Items.end()) {
			/* Unregister the old item from its parents. */
			ConfigItem::Ptr oldItem = it->second;
			ObjectLock olock(oldItem);
			oldItem->UnregisterFromParents();

			/* Steal the old item's children. */
			children = oldItem->GetChildren();
		}

		{
			ObjectLock olock(self);
			self->m_ChildObjects = children;
		}

		/* Register this item. */
		m_Items[ikey] = self;
	}

	if (!dobj) {
		ObjectLock olock(dtype);
		dobj = dtype->GetObject(name);
	}

	/* Register this item with its parents. */
	BOOST_FOREACH(const String& parentName, parents) {
		ConfigItem::Ptr parent = GetObject(type, parentName);
		ObjectLock olock(parent);
		parent->RegisterChild(self);
	}

	/* Create a fake update in the format that
	 * DynamicObject::ApplyUpdate expects. */
	Dictionary::Ptr attrs = boost::make_shared<Dictionary>();

	String key;
	Value data;
	BOOST_FOREACH(tie(key, data), properties) {
		Dictionary::Ptr attr = boost::make_shared<Dictionary>();
		attr->Set("data", data);
		attr->Set("type", Attribute_Config);
		attr->Set("tx", DynamicObject::GetCurrentTx());
		attrs->Set(key, attr);
	}

	Dictionary::Ptr update = boost::make_shared<Dictionary>();
	update->Set("attrs", attrs);
	update->Set("configTx", DynamicObject::GetCurrentTx());

	/* Update or create the object and apply the configuration settings. */
	bool was_null = false;

	if (!dobj) {
		ObjectLock dlock(dtype);
		dobj = dtype->CreateObject(update);
		was_null = true;
	}

	{
		ObjectLock olock(dobj);

		if (!was_null)
			dobj->ApplyUpdate(update, Attribute_Config);

		self->m_DynamicObject = dobj;

		if (dobj->IsAbstract())
			dobj->Unregister();
		else
			dobj->Register();
	}

	/* We need to make a copy of the child objects because the
	 * OnParentCommitted() handler is going to update the list. */
	set<ConfigItem::WeakPtr> children;

	{
		ObjectLock olock(self);
		children = self->m_ChildObjects;
	}

	/* notify our children of the update */
	BOOST_FOREACH(const ConfigItem::WeakPtr wchild, children) {
		const ConfigItem::Ptr& child = wchild.lock();

		if (!child)
			continue;

		child->OnParentCommitted();
	}

	OnCommitted(self);

	return dobj;
}

/**
 * Unregisters the configuration item.
 */
void ConfigItem::Unregister(void)
{
	DynamicObject::Ptr dobj = m_DynamicObject.lock();

	if (dobj) {
		ObjectLock olock(dobj);
		dobj->Unregister();
	}

	ConfigItem::ItemMap::iterator it;
	it = m_Items.find(make_pair(GetType(), GetName()));

	if (it != m_Items.end())
		m_Items.erase(it);

	UnregisterFromParents();

	OnRemoved(GetSelf());
}

void ConfigItem::RegisterChild(const ConfigItem::Ptr& child)
{
	m_ChildObjects.insert(child);
}

void ConfigItem::UnregisterChild(const ConfigItem::Ptr& child)
{
	m_ChildObjects.erase(child);
}

void ConfigItem::UnregisterFromParents(void)
{
	BOOST_FOREACH(const String& parentName, m_Parents) {
		ConfigItem::Ptr parent = GetObject(GetType(), parentName);

		if (parent) {
			ObjectLock olock(parent);
			parent->UnregisterChild(GetSelf());
		}
	}
}

/*
 * Notifies an item that one of its parents has been committed.
 */
void ConfigItem::OnParentCommitted(void)
{
	ConfigItem::Ptr self;

	{
		ObjectLock olock(this);
		self = GetSelf();

		if (GetObject(self->GetType(), self->GetName()) != self)
			return;
	}

	Commit(self);
}

/**
 * Retrieves the DynamicObject that belongs to the configuration item.
 *
 * @returns The DynamicObject.
 */
DynamicObject::Ptr ConfigItem::GetDynamicObject(void) const
{
	return m_DynamicObject.lock();
}

/**
 * Retrieves a configuration item by type and name.
 *
 * @param type The type of the ConfigItem that is to be looked up.
 * @param name The name of the ConfigItem that is to be looked up.
 * @returns The configuration item.
 * @threadsafety Always.
 */
ConfigItem::Ptr ConfigItem::GetObject(const String& type, const String& name)
{
	recursive_mutex::scoped_lock lock(m_Mutex);

	ConfigItem::ItemMap::iterator it;

	it = m_Items.find(make_pair(type, name));

	if (it != m_Items.end())
		return it->second;

	return ConfigItem::Ptr();
}

/**
 * Dumps the config item to the specified stream using Icinga's config item
 * syntax.
 *
 * @param fp The stream.
 */
void ConfigItem::Dump(ostream& fp) const
{
	fp << "object \"" << m_Type << "\" \"" << m_Name << "\"";

	if (m_Parents.size() > 0) {
		fp << " inherits";

		bool first = true;
		BOOST_FOREACH(const String& name, m_Parents) {
			if (!first)
				fp << ",";
			else
				first = false;

			fp << " \"" << name << "\"";
		}
	}

	fp << " {" << "\n";
	m_ExpressionList->Dump(fp, 1);
	fp << "}" << "\n";
}

/**
 * @threadsafety Always.
 */
void ConfigItem::UnloadUnit(const String& unit)
{
	recursive_mutex::scoped_lock lock(m_Mutex);

	Logger::Write(LogInformation, "config", "Unloading config items from compilation unit '" + unit + "'");

	vector<ConfigItem::Ptr> obsoleteItems;

	ConfigItem::Ptr item;
	BOOST_FOREACH(tie(tuples::ignore, item), m_Items) {
		ObjectLock olock(item);

		if (item->GetUnit() != unit)
			continue;

		obsoleteItems.push_back(item);
	}

	BOOST_FOREACH(const ConfigItem::Ptr& item, obsoleteItems) {
		ObjectLock olock(item);
		item->Unregister();
	}
}
