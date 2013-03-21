/*
 * Copyright (c) 2002 Apple Computer, Inc. All Rights Reserved.
 * 
 * The contents of this file constitute Original Code as defined in and are
 * subject to the Apple Public Source License Version 1.2 (the 'License').
 * You may not use this file except in compliance with the License. Please obtain
 * a copy of the License at http://www.apple.com/publicsource and read it before
 * using this file.
 * 
 * This Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESS
 * OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES, INCLUDING WITHOUT
 * LIMITATION, ANY WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 * PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT. Please see the License for the
 * specific language governing rights and limitations under the License.
 */

//
// Access.cpp
//
#include <Security/Access.h>
#include <Security/SecBase.h>
#include "SecBridge.h"
#include <Security/devrandom.h>
#include <Security/uniformrandom.h>
#include <vector>

using namespace KeychainCore;


//
// Create a default Access object.
// This construct an Access with "default form", whatever that happens to be
// in this release.
//
Access::Access(const string &descriptor, const ACL::ApplicationList &trusted)
{
	makeStandard(descriptor, trusted);
}

Access::Access(const string &descriptor)
{
	ACL::ApplicationList trusted;
	trusted.push_back(new TrustedApplication);
	makeStandard(descriptor, trusted);
}

void Access::makeStandard(const string &descriptor, const ACL::ApplicationList &trusted)
{
	// owner "entry"
	RefPointer<ACL> owner = new ACL(*this, descriptor, ACL::defaultSelector);
	owner->setAuthorization(CSSM_ACL_AUTHORIZATION_CHANGE_ACL);
	addOwner(owner);

	// encrypt entry
	RefPointer<ACL> encrypt = new ACL(*this, descriptor, ACL::defaultSelector);
	encrypt->setAuthorization(CSSM_ACL_AUTHORIZATION_ENCRYPT);
	encrypt->form(ACL::allowAllForm);
	add(encrypt);

	// decrypt entry
	RefPointer<ACL> decrypt = new ACL(*this, descriptor, ACL::defaultSelector);
	decrypt->setAuthorization(CSSM_ACL_AUTHORIZATION_DECRYPT);
	decrypt->applications() = trusted;
	add(decrypt);
}


//
// Create an Access object whose initial value is taken
// from a CSSM ACL bearing object.
//
Access::Access(AclBearer &source)
{
	// retrieve and set
	AutoAclOwnerPrototype owner;
	source.getOwner(owner);
	AutoAclEntryInfoList acls;
	source.getAcl(acls);
	compile(*owner, acls.count(), acls.entries());
}


//
// Create an Access object from CSSM-layer access controls
//
Access::Access(const CSSM_ACL_OWNER_PROTOTYPE &owner,
	uint32 aclCount, const CSSM_ACL_ENTRY_INFO *acls)
{
	compile(owner, aclCount, acls);
}


Access::~Access()
{
}


//
// Return all ACL components in a newly-made CFArray.
//
CFArrayRef Access::copySecACLs() const
{
	return makeCFArray(gTypes().acl, mAcls);
}

CFArrayRef Access::copySecACLs(CSSM_ACL_AUTHORIZATION_TAG action) const
{
	list<ACL *> choices;
	for (Map::const_iterator it = mAcls.begin(); it != mAcls.end(); it++)
		if (it->second->authorizations().find(action) != it->second->authorizations().end())
			choices.push_back(it->second);
	return choices.empty() ? NULL : makeCFArray(gTypes().acl, choices);
}


//
// Enter the complete access configuration into a AclBearer.
// If update, skip any part marked unchanged. (If not update, skip
// any part marked deleted.)
//
void Access::setAccess(AclBearer &target, bool update = false)
{
	AclFactory factory;
	editAccess(target, update, factory.promptCred());
}

void Access::setAccess(AclBearer &target, Maker &maker)
{
	// remove initial-setup ACL
	target.deleteAcl(Maker::creationEntryTag, maker.cred());
	
	// insert our own ACL entries
	editAccess(target, false, maker.cred());
}

void Access::editAccess(AclBearer &target, bool update, const AccessCredentials *cred)
{
	assert(mAcls[ownerHandle]);	// have owner
	
	// apply all non-owner ACLs first
	for (Map::iterator it = mAcls.begin(); it != mAcls.end(); it++)
		if (!it->second->isOwner())
			it->second->setAccess(target, update, cred);
	
	// finally, apply owner
	mAcls[ownerHandle]->setAccess(target, update, cred);
}


//
// A convenience function to add one application to a standard ("simple") form
// ACL entry. This will only work if
//  -- there is exactly one ACL entry authorizing the right
//  -- that entry is in simple form
//
void Access::addApplicationToRight(AclAuthorization right, TrustedApplication *app)
{
	vector<ACL *> acls;
	findAclsForRight(right, acls);
	if (acls.size() != 1)
		MacOSError::throwMe(errSecACLNotSimple);	// let's not guess here...
	(*acls.begin())->addApplication(app);
}


//
// Add a new ACL to the resident set. The ACL must have been
// newly made for this Access.
//
void Access::add(ACL *newAcl)
{
	if (&newAcl->access != this)
		MacOSError::throwMe(paramErr);
	assert(!mAcls[newAcl->entryHandle()]);
	mAcls[newAcl->entryHandle()] = newAcl;
}


//
// Add the owner ACL to the resident set. The ACL must have been
// newly made for this Access.
// Since an Access must have exactly one owner ACL, this call
// should only be made (exactly once) for a newly created Access.
//
void Access::addOwner(ACL *newAcl)
{
	newAcl->makeOwner();
	assert(mAcls.find(ownerHandle) == mAcls.end());	// no owner yet
	add(newAcl);
}


//
// Compile a set of ACL entries and owner into internal form.
//
void Access::compile(const CSSM_ACL_OWNER_PROTOTYPE &owner,
	uint32 aclCount, const CSSM_ACL_ENTRY_INFO *acls)
{
	// add owner acl
	mAcls[ownerHandle] = new ACL(*this, AclOwnerPrototype::overlay(owner));
	
	// add acl entries
	const AclEntryInfo *acl = AclEntryInfo::overlay(acls);
	for (uint32 n = 0; n < aclCount; n++) {
		debug("SecAccess", "%p compiling entry %ld", this, acl[n].handle());
		mAcls[acl[n].handle()] = new ACL(*this, acl[n]);
	}
	debug("SecAccess", "%p %ld entries compiled", this, mAcls.size());
}


//
// Creation helper objects
//
const char Access::Maker::creationEntryTag[] = "___setup___";

Access::Maker::Maker(CssmAllocator &alloc)
	: allocator(alloc), mKey(alloc), mCreds(allocator)
{
	// generate random key
	mKey.malloc(keySize);
	UniformRandomBlobs<DevRandomGenerator>().random(mKey.get());
	
	// create entry info for resource creation
	mInput = AclEntryPrototype(TypedList(allocator, CSSM_ACL_SUBJECT_TYPE_PASSWORD,
		new(allocator) ListElement(mKey.get())));
	mInput.proto().tag(creationEntryTag);

	// create credential sample for access
	mCreds += TypedList(allocator, CSSM_SAMPLE_TYPE_PASSWORD, new(allocator) ListElement(mKey.get()));
}

void Access::Maker::initialOwner(ResourceControlContext &ctx, const AccessCredentials *creds)
{
	//@@@ make up ctx.entry-info
	ctx.input() = mInput;
	ctx.credentials(creds);
}

const AccessCredentials *Access::Maker::cred()
{
	return &mCreds;
}