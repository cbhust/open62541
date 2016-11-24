#include "ua_server_internal.h"
#include "ua_services.h"

/**********************/
/* Consistency Checks */
/**********************/

/* Check if the requested parent node exists, has the right node class and is
 * referenced with an allowed (hierarchical) reference type. For "type" nodes,
 * only hasSubType references are allowed. */
static UA_StatusCode
checkParentReference(UA_Server *server, UA_Session *session, UA_NodeClass nodeClass,
                     const UA_NodeId *parentNodeId, const UA_NodeId *referenceTypeId) {
    /* See if the parent exists */
    const UA_Node *parent = UA_NodeStore_get(server->nodestore, parentNodeId);
    if(!parent) {
        UA_LOG_INFO_SESSION(server->config.logger, session,
                             "AddNodes: Parent node not found");
        return UA_STATUSCODE_BADPARENTNODEIDINVALID;
    }

    /* Check the referencetype exists */
    const UA_ReferenceTypeNode *referenceType =
        (const UA_ReferenceTypeNode*)UA_NodeStore_get(server->nodestore, referenceTypeId);
    if(!referenceType) {
        UA_LOG_INFO_SESSION(server->config.logger, session,
                             "AddNodes: Reference type to the parent not found");
        return UA_STATUSCODE_BADREFERENCETYPEIDINVALID;
    }

    /* Check if the referencetype is a reference type node */
    if(referenceType->nodeClass != UA_NODECLASS_REFERENCETYPE) {
        UA_LOG_INFO_SESSION(server->config.logger, session,
                             "AddNodes: Reference type to the parent invalid");
        return UA_STATUSCODE_BADREFERENCETYPEIDINVALID;
    }

    /* Check that the reference type is not abstract */
    if(referenceType->isAbstract == true) {
        UA_LOG_INFO_SESSION(server->config.logger, session,
                             "AddNodes: Abstract reference type to the parent invalid");
        return UA_STATUSCODE_BADREFERENCENOTALLOWED;
    }

    /* Check hassubtype relation for type nodes */
    const UA_NodeId subtypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_HASSUBTYPE);
    if(nodeClass == UA_NODECLASS_DATATYPE ||
       nodeClass == UA_NODECLASS_VARIABLETYPE ||
       nodeClass == UA_NODECLASS_OBJECTTYPE ||
       nodeClass == UA_NODECLASS_REFERENCETYPE) {
        /* type needs hassubtype reference to the supertype */
        if(!UA_NodeId_equal(referenceTypeId, &subtypeId)) {
            UA_LOG_INFO_SESSION(server->config.logger, session,
                                 "AddNodes: New type node need to have a "
                                 "hassubtype reference");
            return UA_STATUSCODE_BADREFERENCENOTALLOWED;
        }
        /* supertype needs to be of the same node type  */
        if(parent->nodeClass != nodeClass) {
            UA_LOG_INFO_SESSION(server->config.logger, session,
                                 "AddNodes: New type node needs to be of the same "
                                 "node type as the parent");
            return UA_STATUSCODE_BADPARENTNODEIDINVALID;
        }
        return UA_STATUSCODE_GOOD;
    }

    /* Test if the referencetype is hierarchical */
    const UA_NodeId hierarchicalReference =
        UA_NODEID_NUMERIC(0, UA_NS0ID_HIERARCHICALREFERENCES);
    if(!isNodeInTree(server->nodestore, referenceTypeId,
                     &hierarchicalReference, &subtypeId, 1)) {
        UA_LOG_INFO_SESSION(server->config.logger, session,
                             "AddNodes: Reference to the parent is not hierarchical");
        return UA_STATUSCODE_BADREFERENCETYPEIDINVALID;
    }

    return UA_STATUSCODE_GOOD;
}

/* Check the consistency of the variable (or variable type) attributes data
 * type, value rank, array dimensions internally and against the parent variable
 * type. */
static UA_StatusCode
typeCheckVariableNode(UA_Server *server, UA_Session *session, UA_VariableNode *node,
                      const UA_NodeId *typeDef) {
    /* Workaround if no datatype is set */
    if(UA_NodeId_isNull(&node->dataType)) {
        UA_LOG_INFO_SESSION(server->config.logger, session,
                            "TypeCheck: No datatype of Variable(Type) defined; "
                            "Set to BaseDataType.");
        node->dataType = UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATATYPE);
    }

    /* Omit some type checks for ns0 generation */
    const UA_NodeId baseDataVariableType = UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE);
    if(UA_NodeId_equal(&node->nodeId, &baseDataVariableType))
        return UA_STATUSCODE_GOOD;

    /* Get the variable type */
    const UA_VariableTypeNode *vt =
        (const UA_VariableTypeNode*)UA_NodeStore_get(server->nodestore, typeDef);
    if(!vt || vt->nodeClass != UA_NODECLASS_VARIABLETYPE)
        return UA_STATUSCODE_BADTYPEDEFINITIONINVALID;
    if(node->nodeClass == UA_NODECLASS_VARIABLE && vt->isAbstract)
        return UA_STATUSCODE_BADTYPEDEFINITIONINVALID;

    /* Check the datatype against the vt */
    UA_StatusCode retval = UA_STATUSCODE_GOOD;
    const UA_NodeId subtypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_HASSUBTYPE);
    if(!isNodeInTree(server->nodestore, &node->dataType, &vt->dataType, &subtypeId, 1))
        return UA_STATUSCODE_BADTYPEMISMATCH;

    /* We need the value for some checks. Might come from a datasource. */
    UA_DataValue value;
    UA_DataValue_init(&value);
    retval = readValueAttribute(server, node, &value);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;

    /* Workaround: If there is no value but the type is concrete, create an
     * "empty" value */
    if(!value.value.type) {
        const UA_DataType *type = UA_findDataType(&node->dataType);
        if(type) {
            UA_LOG_INFO_SESSION(server->config.logger, session,
                                "TypeCheck: Value of Variable(Type) is empty. "
                                "But this is only allowed for BaseDataType. "
                                "Create a \"null\" value.");
            UA_Variant v;
            UA_Variant_init(&v);
            if(node->valueRank == 1)
                UA_Variant_setArray(&v, UA_EMPTY_ARRAY_SENTINEL, 0, type);
            else {
                void *data = UA_alloca(type->memSize);
                UA_init(data, type);
                UA_Variant_setScalar(&v, data, type);
            }
            UA_Server_writeValue(server, node->nodeId, v);
            v.storageType = UA_VARIANT_DATA_NODELETE;
            value.value = v;
        }
    }

    /* Get the array dimensions */
    size_t arrayDims = node->arrayDimensionsSize;
    if(arrayDims == 0) {
        if(value.hasValue && UA_Variant_isScalar(&value.value) && node->valueRank == 0) {
            /* Workaround the user forgetting to set the value rank */
            UA_LOG_INFO_SESSION(server->config.logger, session,
                                "TypeCheck: The value rank does not match the data; "
                                "Using the value rank of the variable type.");
            node->valueRank = vt->valueRank;
        } else if(value.hasValue && value.value.type &&
                  !UA_Variant_isScalar(&value.value) && node->valueRank == 1) {
            /* No array dimensions on an array implies one dimensions*/
            arrayDims = 1;
        }
    }

    UA_DataValue_deleteMembers(&value); /* Free the value before any return clause */

    /* Check valueRank against array dimensions */
    retval = compatibleValueRankArrayDimensions(node->valueRank, arrayDims);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;

    /* Check valueRank against the vt */
    retval = compatibleValueRanks(node->valueRank, vt->valueRank);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;

    /* Check array dimensions against the vt */
    retval = compatibleArrayDimensions(node->arrayDimensionsSize, node->arrayDimensions,
                                        vt->arrayDimensionsSize, vt->arrayDimensions);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;

    /* This internally converts the value to a valid type if possible */
    if(node->valueSource == UA_VALUESOURCE_DATA)
        retval = typeCheckValue(server, &node->dataType, node->valueRank,
                                node->arrayDimensionsSize, node->arrayDimensions,
                                &node->value.data.value.value, NULL, &node->value.data.value.value);
    return retval;
}

static UA_StatusCode
typeCheckNode(UA_Server *server, UA_Session *session, const UA_NodeId *nodeId,
              UA_NodeClass nodeClass, const UA_NodeId *parentId, const UA_NodeId *typeId) {
    const UA_NodeId *typeParent;
    if(nodeClass == UA_NODECLASS_VARIABLE)
        typeParent = typeId;
    else if(nodeClass == UA_NODECLASS_VARIABLETYPE)
        typeParent = parentId;
    else
        return UA_STATUSCODE_GOOD;
    return UA_Server_editNode(server, session, nodeId,
                              (UA_EditNodeCallback)typeCheckVariableNode, typeParent);
}

/********************/
/* Instantiate Node */
/********************/

static UA_StatusCode
setObjectInstanceHandle(UA_Server *server, UA_Session *session, UA_ObjectNode* node,
                        void * (*constructor)(const UA_NodeId instance)) {
    if(node->nodeClass != UA_NODECLASS_OBJECT)
        return UA_STATUSCODE_BADNODECLASSINVALID;
    if(!node->instanceHandle)
        node->instanceHandle = constructor(node->nodeId);
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
copyChildNodes(UA_Server *server, UA_Session *session, 
               const UA_NodeId *sourceNodeId, const UA_NodeId *destinationNodeId, 
               UA_InstantiationCallback *instantiationCallback);

static UA_StatusCode
instantiateNode(UA_Server *server, UA_Session *session, const UA_NodeId *nodeId,
                UA_NodeClass nodeClass, const UA_NodeId *typeId,
                UA_InstantiationCallback *instantiationCallback) {
    /* Currently, only variables and objects are instantiated */
    if(nodeClass != UA_NODECLASS_VARIABLE &&
       nodeClass != UA_NODECLASS_OBJECT)
        return UA_STATUSCODE_GOOD;
        
    /* Get the type node */
    const UA_Node *typenode = UA_NodeStore_get(server->nodestore, typeId);
    if(!typenode)
        return UA_STATUSCODE_BADTYPEDEFINITIONINVALID;

    /* See if the type has the correct node class */
    if(nodeClass == UA_NODECLASS_VARIABLE) {
        if(typenode->nodeClass != UA_NODECLASS_VARIABLETYPE ||
           ((const UA_VariableTypeNode*)typenode)->isAbstract)
            return UA_STATUSCODE_BADTYPEDEFINITIONINVALID;
    } else { /* nodeClass == UA_NODECLASS_OBJECT */
        if(typenode->nodeClass != UA_NODECLASS_OBJECTTYPE ||
           ((const UA_ObjectTypeNode*)typenode)->isAbstract)
            return UA_STATUSCODE_BADTYPEDEFINITIONINVALID;
    }

    /* Get the hierarchy of the type and all its supertypes */
    UA_NodeId *hierarchy = NULL;
    size_t hierarchySize = 0;
    UA_StatusCode retval = getTypeHierarchy(server->nodestore, typenode,
                                            true, &hierarchy, &hierarchySize);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;
    
    /* Copy members of the type and supertypes */
    for(size_t i = 0; i < hierarchySize; ++i)
        retval |= copyChildNodes(server, session, &hierarchy[i],
                                 nodeId, instantiationCallback);
    UA_Array_delete(hierarchy, hierarchySize, &UA_TYPES[UA_TYPES_NODEID]);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;

    /* Call the object constructor */
    if(typenode->nodeClass == UA_NODECLASS_OBJECTTYPE) {
        const UA_ObjectLifecycleManagement *olm =
            &((const UA_ObjectTypeNode*)typenode)->lifecycleManagement;
        if(olm->constructor)
            UA_Server_editNode(server, session, nodeId,
                               (UA_EditNodeCallback)setObjectInstanceHandle,
                               olm->constructor);
    }

    /* Add a hasType reference */
    UA_AddReferencesItem addref;
    UA_AddReferencesItem_init(&addref);
    addref.sourceNodeId = *nodeId;
    addref.referenceTypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_HASTYPEDEFINITION);
    addref.isForward = true;
    addref.targetNodeId.nodeId = *typeId;
    retval = Service_AddReferences_single(server, session, &addref);

    /* Call custom callback */
    if(retval == UA_STATUSCODE_GOOD && instantiationCallback)
        instantiationCallback->method(*nodeId, *typeId, instantiationCallback->handle);
    return retval;
}

/* Search for an instance of "browseName" in node searchInstance Used during
 * copyChildNodes to find overwritable/mergable nodes */
static UA_StatusCode
instanceFindAggregateByBrowsename(UA_Server *server, UA_Session *session, 
                                  const UA_NodeId *searchInstance, 
                                  const UA_QualifiedName *browseName,
                                  UA_NodeId *outInstanceNodeId) {
    UA_BrowseDescription bd;
    UA_BrowseDescription_init(&bd);
    bd.nodeId = *searchInstance;
    bd.referenceTypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_AGGREGATES);
    bd.includeSubtypes = true;
    bd.browseDirection = UA_BROWSEDIRECTION_FORWARD;
    bd.nodeClassMask = UA_NODECLASS_OBJECT | UA_NODECLASS_VARIABLE | UA_NODECLASS_METHOD;
    bd.resultMask = UA_BROWSERESULTMASK_NODECLASS | UA_BROWSERESULTMASK_BROWSENAME;
    
    UA_BrowseResult br;
    UA_BrowseResult_init(&br);
    Service_Browse_single(server, session, NULL, &bd, 0, &br);
    if(br.statusCode != UA_STATUSCODE_GOOD)
        return br.statusCode;
    
    UA_StatusCode retval = UA_STATUSCODE_GOOD;
    for(size_t i = 0; i < br.referencesSize; ++i) {
        UA_ReferenceDescription *rd = &br.references[i];
        if(rd->browseName.namespaceIndex == browseName->namespaceIndex &&
           UA_String_equal(&rd->browseName.name, &browseName->name)) {
            retval = UA_NodeId_copy(&rd->nodeId.nodeId, outInstanceNodeId);
            break;
        }
    }
    
    UA_BrowseResult_deleteMembers(&br);
    return retval;
}

/* Copy any children of Node sourceNodeId to another node destinationNodeId. */
static UA_StatusCode
copyChildNodes(UA_Server *server, UA_Session *session, 
               const UA_NodeId *sourceNodeId, const UA_NodeId *destinationNodeId, 
               UA_InstantiationCallback *instantiationCallback) {
    /* Browse to get all children */
    UA_BrowseDescription bd;
    UA_BrowseDescription_init(&bd);
    bd.nodeId = *sourceNodeId;
    bd.referenceTypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_AGGREGATES);
    bd.includeSubtypes = true;
    bd.browseDirection = UA_BROWSEDIRECTION_FORWARD;
    bd.nodeClassMask = UA_NODECLASS_OBJECT | UA_NODECLASS_VARIABLE | UA_NODECLASS_METHOD;
    bd.resultMask = UA_BROWSERESULTMASK_REFERENCETYPEID | UA_BROWSERESULTMASK_NODECLASS |
        UA_BROWSERESULTMASK_BROWSENAME;

    UA_BrowseResult br;
    UA_BrowseResult_init(&br);
    Service_Browse_single(server, session, NULL, &bd, 0, &br);
    if(br.statusCode != UA_STATUSCODE_GOOD)
        return br.statusCode;
  
    /* Copy all children */
    UA_StatusCode retval = UA_STATUSCODE_GOOD;
    for(size_t i = 0; i < br.referencesSize; ++i) {
        UA_ReferenceDescription *rd = &br.references[i];
        UA_NodeId existingChild = UA_NODEID_NULL;
        retval = instanceFindAggregateByBrowsename(server, session, destinationNodeId,
                                                   &rd->browseName, &existingChild);
        if(retval != UA_STATUSCODE_GOOD)
            break;
        
        /* No existing child with that browsename. Create it. */
        if(UA_NodeId_isNull(&existingChild)) {
            if(rd->nodeClass == UA_NODECLASS_METHOD) {
                /* Add a reference to the method in the objecttype */
                UA_AddReferencesItem newItem;
                UA_AddReferencesItem_init(&newItem);
                newItem.sourceNodeId = *destinationNodeId;
                newItem.referenceTypeId = rd->referenceTypeId;
                newItem.isForward = true;
                newItem.targetNodeId = rd->nodeId;
                newItem.targetNodeClass = UA_NODECLASS_METHOD;
                retval = Service_AddReferences_single(server, session, &newItem);
            } else if(rd->nodeClass == UA_NODECLASS_VARIABLE ||
                      rd->nodeClass == UA_NODECLASS_OBJECT) {
                /* Copy the node */
                UA_Node *node = UA_NodeStore_getCopy(server->nodestore, &rd->nodeId.nodeId);
                if(!node) {
                    retval = UA_STATUSCODE_BADNODEIDINVALID;
                    break;
                }

                /* Reset the NodeId (random id will be assigned in the nodestore */
                UA_NodeId_deleteMembers(&node->nodeId);
                node->nodeId.namespaceIndex = destinationNodeId->namespaceIndex;
                
                /* Get the node type */
                const UA_NodeId *typeId = NULL;
                if(node->nodeClass == UA_NODECLASS_VARIABLE ||
                   node->nodeClass == UA_NODECLASS_OBJECT) {
                    const UA_Node *vt = getNodeType(server, node);
                    if(vt)
                        typeId = &vt->nodeId;
                }

                /* Add the node (instantiates internally) */
                retval = UA_Server_addNode(server, session, node, destinationNodeId,
                                           &rd->referenceTypeId, typeId,
                                           instantiationCallback, &existingChild);
                if(retval != UA_STATUSCODE_GOOD)
                    break;
            }
            continue;
        }

        /* Have a child with that browseName. Try to deep-copy missing members. */
        if(rd->nodeClass == UA_NODECLASS_VARIABLE ||
           rd->nodeClass == UA_NODECLASS_OBJECT)
            retval = copyChildNodes(server, session, &rd->nodeId.nodeId,
                                    &existingChild, instantiationCallback);
        UA_NodeId_deleteMembers(&existingChild);
        if(retval != UA_STATUSCODE_GOOD)
            break;
    }
    UA_BrowseResult_deleteMembers(&br);
    return retval;
}

/*******************************************/
/* Create nodes from attribute description */
/*******************************************/

static UA_StatusCode
copyStandardAttributes(UA_Node *node, const UA_AddNodesItem *item,
                       const UA_NodeAttributes *attr) {
    UA_StatusCode retval;
    retval  = UA_NodeId_copy(&item->requestedNewNodeId.nodeId, &node->nodeId);
    retval |= UA_QualifiedName_copy(&item->browseName, &node->browseName);
    retval |= UA_LocalizedText_copy(&attr->displayName, &node->displayName);
    retval |= UA_LocalizedText_copy(&attr->description, &node->description);
    node->writeMask = attr->writeMask;
    node->userWriteMask = attr->userWriteMask;
    return retval;
}

static UA_StatusCode
copyCommonVariableAttributes(UA_VariableNode *node, const UA_VariableAttributes *attr) {
    /* Copy the array dimensions */
    UA_StatusCode retval = UA_Array_copy(attr->arrayDimensions, attr->arrayDimensionsSize,
                                         (void**)&node->arrayDimensions, &UA_TYPES[UA_TYPES_UINT32]);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;
    node->arrayDimensionsSize = attr->arrayDimensionsSize;

    /* Data type and value rank */
    retval |= UA_NodeId_copy(&attr->dataType, &node->dataType);
    node->valueRank = attr->valueRank;

    /* Copy the value */
    node->valueSource = UA_VALUESOURCE_DATA;
    retval |= UA_Variant_copy(&attr->value, &node->value.data.value.value);
    node->value.data.value.hasValue = true;

    return retval;
}

static UA_StatusCode
copyVariableNodeAttributes(UA_VariableNode *vnode, const UA_VariableAttributes *attr) {
    vnode->accessLevel = attr->accessLevel;
    vnode->userAccessLevel = attr->userAccessLevel;
    vnode->historizing = attr->historizing;
    vnode->minimumSamplingInterval = attr->minimumSamplingInterval;
    return copyCommonVariableAttributes(vnode, attr);
}

static UA_StatusCode
copyVariableTypeNodeAttributes(UA_VariableTypeNode *vtnode,
                               const UA_VariableTypeAttributes *attr) {
    vtnode->isAbstract = attr->isAbstract;
    return copyCommonVariableAttributes((UA_VariableNode*)vtnode,
                                        (const UA_VariableAttributes*)attr);
}

static UA_StatusCode
copyObjectNodeAttributes(UA_ObjectNode *onode, const UA_ObjectAttributes *attr) {
    onode->eventNotifier = attr->eventNotifier;
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
copyReferenceTypeNodeAttributes(UA_ReferenceTypeNode *rtnode,
                                const UA_ReferenceTypeAttributes *attr) {
    rtnode->isAbstract = attr->isAbstract;
    rtnode->symmetric = attr->symmetric;
    return UA_LocalizedText_copy(&attr->inverseName, &rtnode->inverseName);
}

static UA_StatusCode
copyObjectTypeNodeAttributes(UA_ObjectTypeNode *otnode,
                             const UA_ObjectTypeAttributes *attr) {
    otnode->isAbstract = attr->isAbstract;
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
copyViewNodeAttributes(UA_ViewNode *vnode, const UA_ViewAttributes *attr) {
    vnode->containsNoLoops = attr->containsNoLoops;
    vnode->eventNotifier = attr->eventNotifier;
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
copyDataTypeNodeAttributes(UA_DataTypeNode *dtnode,
                           const UA_DataTypeAttributes *attr) {
    dtnode->isAbstract = attr->isAbstract;
    return UA_STATUSCODE_GOOD;
}

#define CHECK_ATTRIBUTES(TYPE)                                          \
    if(item->nodeAttributes.content.decoded.type != &UA_TYPES[UA_TYPES_##TYPE]) { \
        retval = UA_STATUSCODE_BADNODEATTRIBUTESINVALID;                \
        break;                                                          \
    }

/* Copy the attributes into a new node. On success, newNode points to the
 * created node */
static UA_StatusCode
createNodeFromAttributes(UA_Server *server, const UA_AddNodesItem *item,
                         UA_Node **newNode) {
    /* Check that we can read the attributes */
    if(item->nodeAttributes.encoding < UA_EXTENSIONOBJECT_DECODED ||
       !item->nodeAttributes.content.decoded.type)
        return UA_STATUSCODE_BADNODEATTRIBUTESINVALID;

    /* Create the node */
    // todo: error case where the nodeclass is faulty
    void *node = UA_NodeStore_newNode(item->nodeClass);
    if(!node)
        return UA_STATUSCODE_BADOUTOFMEMORY;

    /* Copy the attributes into the node */
    void *data = item->nodeAttributes.content.decoded.data;
    UA_StatusCode retval = copyStandardAttributes(node, item, data);
    switch(item->nodeClass) {
    case UA_NODECLASS_OBJECT:
        CHECK_ATTRIBUTES(OBJECTATTRIBUTES);
        retval |= copyObjectNodeAttributes(node, data);
        break;
    case UA_NODECLASS_VARIABLE:
        CHECK_ATTRIBUTES(VARIABLEATTRIBUTES);
        retval |= copyVariableNodeAttributes(node, data);
        break;
    case UA_NODECLASS_OBJECTTYPE:
        CHECK_ATTRIBUTES(OBJECTTYPEATTRIBUTES);
        retval |= copyObjectTypeNodeAttributes(node, data);
        break;
    case UA_NODECLASS_VARIABLETYPE:
        CHECK_ATTRIBUTES(VARIABLETYPEATTRIBUTES);
        retval |= copyVariableTypeNodeAttributes(node, data);
        break;
    case UA_NODECLASS_REFERENCETYPE:
        CHECK_ATTRIBUTES(REFERENCETYPEATTRIBUTES);
        retval |= copyReferenceTypeNodeAttributes(node, data);
        break;
    case UA_NODECLASS_DATATYPE:
        CHECK_ATTRIBUTES(DATATYPEATTRIBUTES);
        retval |= copyDataTypeNodeAttributes(node, data);
        break;
    case UA_NODECLASS_VIEW:
        CHECK_ATTRIBUTES(VIEWATTRIBUTES);
        retval |= copyViewNodeAttributes(node, data);
        break;
    case UA_NODECLASS_METHOD:
    case UA_NODECLASS_UNSPECIFIED:
    default:
        retval = UA_STATUSCODE_BADNODECLASSINVALID;
    }

    if(retval == UA_STATUSCODE_GOOD)
        *newNode = node;
    else
        UA_NodeStore_deleteNode(node);
    return retval;
}

/************/
/* Add Node */
/************/

UA_StatusCode
UA_Server_addNode_begin(UA_Server *server, UA_Session *session,
                        UA_Node *node, UA_NodeId *addedNodeId) {
    /* Check the namespaceindex */
    if(node->nodeId.namespaceIndex >= server->namespacesSize) {
        UA_LOG_INFO_SESSION(server->config.logger, session,
                            "AddNodes: Namespace invalid");
        UA_NodeStore_deleteNode(node);
        return UA_STATUSCODE_BADNODEIDINVALID;
    }

    /* Add the node to the nodestore */
    UA_StatusCode retval = UA_NodeStore_insert(server->nodestore, node);
    if(retval != UA_STATUSCODE_GOOD) {
        UA_LOG_INFO_SESSION(server->config.logger, session,
                             "AddNodes: Node could not be added to the nodestore "
                             "with error code %s", UA_StatusCode_name(retval));
        return retval;
    }

    /* Copy the nodeid if needed */
    if(addedNodeId) {
        retval = UA_NodeId_copy(&node->nodeId, addedNodeId);
        if(retval != UA_STATUSCODE_GOOD) {
            UA_LOG_INFO_SESSION(server->config.logger, session,
                                 "AddNodes: Could not copy the nodeid");
            UA_NodeStore_remove(server->nodestore, &node->nodeId);
        }
    }
    return retval;
}

UA_StatusCode
UA_Server_addNode_finish(UA_Server *server, UA_Session *session, const UA_NodeId *nodeId,
                         UA_NodeClass nodeClass, const UA_NodeId *parentNodeId,
                         const UA_NodeId *referenceTypeId, const UA_NodeId *typeDefinition,
                         UA_InstantiationCallback *instantiationCallback) {
    UA_StatusCode retval = UA_STATUSCODE_GOOD;

    /* Check parent reference. Objects may have no parent. */
    if(nodeClass != UA_NODECLASS_OBJECT || !UA_NodeId_isNull(parentNodeId) || !UA_NodeId_isNull(referenceTypeId)) {
        retval = checkParentReference(server, session, nodeClass, parentNodeId, referenceTypeId);
        if(retval != UA_STATUSCODE_GOOD) {
            UA_LOG_INFO_SESSION(server->config.logger, session,
                                "AddNodes: Parent reference invalid");
            goto cleanup;
        }
    }

    /* Use standard type definition if none defined */
    const UA_NodeId baseDataVariableType = UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE);
    const UA_NodeId baseObjectType = UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE);
    if(UA_NodeId_isNull(typeDefinition)) {
        if(nodeClass == UA_NODECLASS_VARIABLE)
            typeDefinition = &baseDataVariableType;
        else if(nodeClass == UA_NODECLASS_OBJECT)
            typeDefinition = &baseObjectType;
    }
    
    /* Type check node */
    retval = typeCheckNode(server, session, nodeId, nodeClass,
                           parentNodeId, typeDefinition);
    if(retval != UA_STATUSCODE_GOOD) {
        UA_LOG_INFO_SESSION(server->config.logger, session,
                            "AddNodes: Type checking failed");
        goto cleanup;
    }

    /* Instantiate node */
    retval = instantiateNode(server, session, nodeId, nodeClass,
                             typeDefinition, instantiationCallback);
    if(retval != UA_STATUSCODE_GOOD) {
        UA_LOG_INFO_SESSION(server->config.logger, session,
                            "AddNodes: Node instantiation failed "
                            "with code %s", UA_StatusCode_name(retval));
        goto cleanup;
    }

    /* Add parent reference */
    if(!UA_NodeId_isNull(parentNodeId)) {
        UA_AddReferencesItem item;
        UA_AddReferencesItem_init(&item);
        item.sourceNodeId = *nodeId;
        item.referenceTypeId = *referenceTypeId;
        item.isForward = false;
        item.targetNodeId.nodeId = *parentNodeId;
        retval = Service_AddReferences_single(server, session, &item);
        if(retval != UA_STATUSCODE_GOOD) {
            UA_LOG_INFO_SESSION(server->config.logger, session,
                                "AddNodes: Adding reference to parent failed");
            goto cleanup;
        }
    }

    return UA_STATUSCODE_GOOD;

 cleanup:
    if(retval != UA_STATUSCODE_GOOD)
        Service_DeleteNodes_single(server, &adminSession, nodeId, true);
    return retval;
}

UA_StatusCode
UA_Server_addNode(UA_Server *server, UA_Session *session, UA_Node *node,
                  const UA_NodeId *parentNodeId, const UA_NodeId *referenceTypeId,
                  const UA_NodeId *typeDefinition,
                  UA_InstantiationCallback *instantiationCallback,
                  UA_NodeId *addedNodeId) {
    /* Store the NodeId internally if the user does not want to receive it */
    UA_NodeId newNodeId;
    if(!addedNodeId) {
        UA_NodeId_init(&newNodeId);
        addedNodeId = &newNodeId;
    }

    /* Add to the nodestore */
    UA_StatusCode retval = UA_Server_addNode_begin(server, session, node, addedNodeId);
    if(retval != UA_STATUSCODE_GOOD) {
        UA_NodeId_deleteMembers(addedNodeId);
        return retval;
    }

    /* Typecheck, validate and instantiate the node. Removes node internally if
     * not successful. */
    retval = UA_Server_addNode_finish(server, session, addedNodeId, node->nodeClass, parentNodeId,
                                      referenceTypeId, typeDefinition, instantiationCallback);

    /* Clean up */
    if(retval != UA_STATUSCODE_GOOD || addedNodeId == &newNodeId)
        UA_NodeId_deleteMembers(addedNodeId);
    return retval;
}

static void
Service_AddNodes_single(UA_Server *server, UA_Session *session,
                        const UA_AddNodesItem *item, UA_AddNodesResult *result,
                        UA_InstantiationCallback *instantiationCallback) {
    /* Create the node from the attributes*/
    UA_Node *node = NULL;
    result->statusCode = createNodeFromAttributes(server, item, &node);
    if(result->statusCode != UA_STATUSCODE_GOOD)
        return;
    UA_assert(node != NULL);

    /* Run consistency checks and add the node */
    result->statusCode = UA_Server_addNode(server, session, node, &item->parentNodeId.nodeId,
                                           &item->referenceTypeId, &item->typeDefinition.nodeId,
                                           instantiationCallback, &result->addedNodeId);
}

void Service_AddNodes(UA_Server *server, UA_Session *session,
                      const UA_AddNodesRequest *request, UA_AddNodesResponse *response) {
    UA_LOG_DEBUG_SESSION(server->config.logger, session,
                         "Processing AddNodesRequest");
    if(request->nodesToAddSize <= 0) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADNOTHINGTODO;
        return;
    }
    size_t size = request->nodesToAddSize;

    response->results = UA_Array_new(size, &UA_TYPES[UA_TYPES_ADDNODESRESULT]);
    if(!response->results) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADOUTOFMEMORY;
        return;
    }

#ifdef UA_ENABLE_EXTERNAL_NAMESPACES
#ifdef _MSC_VER
    UA_Boolean *isExternal = UA_alloca(size);
    UA_UInt32 *indices = UA_alloca(sizeof(UA_UInt32)*size);
#else
    UA_Boolean isExternal[size];
    UA_UInt32 indices[size];
#endif
    memset(isExternal, false, sizeof(UA_Boolean) * size);
    for(size_t j = 0; j <server->externalNamespacesSize; ++j) {
        size_t indexSize = 0;
        for(size_t i = 0;i < size;++i) {
            if(request->nodesToAdd[i].requestedNewNodeId.nodeId.namespaceIndex !=
               server->externalNamespaces[j].index)
                continue;
            isExternal[i] = true;
            indices[indexSize] = (UA_UInt32)i;
            ++indexSize;
        }
        if(indexSize == 0)
            continue;
        UA_ExternalNodeStore *ens = &server->externalNamespaces[j].externalNodeStore;
        ens->addNodes(ens->ensHandle, &request->requestHeader,
                      request->nodesToAdd, indices, (UA_UInt32)indexSize,
                      response->results, response->diagnosticInfos);
    }
#endif

    response->resultsSize = size;
    for(size_t i = 0; i < size; ++i) {
#ifdef UA_ENABLE_EXTERNAL_NAMESPACES
        if(!isExternal[i])
#endif
            Service_AddNodes_single(server, session, &request->nodesToAdd[i],
                                    &response->results[i], NULL);
    }
}

UA_StatusCode
__UA_Server_addNode(UA_Server *server, UA_NodeClass nodeClass,
                    const UA_NodeId *requestedNewNodeId, const UA_NodeId *parentNodeId,
                    const UA_NodeId *referenceTypeId, const UA_QualifiedName *browseName,
                    const UA_NodeId *typeDefinition, const UA_NodeAttributes *attr,
                    const UA_DataType *attributeType,
                    UA_InstantiationCallback *instantiationCallback, UA_NodeId *outNewNodeId) {
    /* Create the node from the attributes*/
    UA_Node *node = NULL;
    UA_AddNodesItem item;
    UA_AddNodesItem_init(&item);
    item.requestedNewNodeId.nodeId = *requestedNewNodeId;
    item.browseName = *browseName;
    item.nodeClass = nodeClass;
    item.nodeAttributes = (UA_ExtensionObject){
        .encoding = UA_EXTENSIONOBJECT_DECODED_NODELETE,
        .content.decoded = {attributeType, (void*)(uintptr_t)attr}};
    UA_StatusCode retval = createNodeFromAttributes(server, &item, &node);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;
    UA_assert(node != NULL);

    /* Run the normal addnodes service */
    return UA_Server_addNode(server, &adminSession, node,
                             parentNodeId, referenceTypeId, typeDefinition,
                             instantiationCallback, outNewNodeId);
}

UA_StatusCode
__UA_Server_addNode_begin(UA_Server *server, const UA_NodeClass nodeClass,
                          const UA_NodeId *requestedNewNodeId, const UA_QualifiedName *browseName,
                          const UA_NodeAttributes *attr, const UA_DataType *attributeType,
                          UA_NodeId *outNewNodeId) {
    /* Create the node from the attributes*/
    UA_Node *node = NULL;
    UA_AddNodesItem item;
    UA_AddNodesItem_init(&item);
    item.requestedNewNodeId.nodeId = *requestedNewNodeId;
    item.browseName = *browseName;
    item.nodeClass = nodeClass;
    item.nodeAttributes = (UA_ExtensionObject){
        .encoding = UA_EXTENSIONOBJECT_DECODED_NODELETE,
        .content.decoded = {attributeType, (void*)(uintptr_t)attr}};
    UA_StatusCode retval = createNodeFromAttributes(server, &item, &node);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;
    UA_assert(node != NULL);

    /* Add the node without checks or instantiation */
    return UA_Server_addNode_begin(server, &adminSession, node, outNewNodeId);
}

UA_StatusCode
__UA_Server_addNode_finish(UA_Server *server, const UA_NodeId *nodeId,
                           UA_NodeClass nodeClass, const UA_NodeId *parentNodeId,
                           const UA_NodeId *referenceTypeId, const UA_NodeId *typeDefinition,
                           UA_InstantiationCallback *instantiationCallback) {
    return UA_Server_addNode_finish(server, &adminSession, nodeId, nodeClass,
                                    parentNodeId, referenceTypeId,
                                    typeDefinition, instantiationCallback);
}

/**************************************************/
/* Add Special Nodes (not possible over the wire) */
/**************************************************/

UA_StatusCode
UA_Server_addDataSourceVariableNode(UA_Server *server, const UA_NodeId requestedNewNodeId,
                                    const UA_NodeId parentNodeId, const UA_NodeId referenceTypeId,
                                    const UA_QualifiedName browseName, const UA_NodeId typeDefinition,
                                    const UA_VariableAttributes attr, const UA_DataSource dataSource,
                                    UA_NodeId *outNewNodeId) {
    /* Create the new node */
    UA_VariableNode *node = UA_NodeStore_newVariableNode();
    if(!node)
        return UA_STATUSCODE_BADOUTOFMEMORY;

    /* Read the current value (to do typechecking) */
    UA_StatusCode retval = UA_STATUSCODE_GOOD;
    UA_VariableAttributes editAttr = attr;
    UA_DataValue value;
    UA_DataValue_init(&value);
    if(dataSource.read)
        retval = dataSource.read(dataSource.handle, requestedNewNodeId,
                                 false, NULL, &value);
    else
        retval = UA_STATUSCODE_BADTYPEMISMATCH;
    editAttr.value = value.value;

    if(retval != UA_STATUSCODE_GOOD) {
        UA_NodeStore_deleteNode((UA_Node*)node);
        return retval;
    }

    /* Copy attributes into node */
    UA_RCU_LOCK();
    UA_AddNodesItem item;
    UA_AddNodesItem_init(&item);
    item.requestedNewNodeId.nodeId = requestedNewNodeId;
    item.browseName = browseName;
    item.typeDefinition.nodeId = typeDefinition;
    item.parentNodeId.nodeId = parentNodeId;
    retval |= copyStandardAttributes((UA_Node*)node, &item, (const UA_NodeAttributes*)&editAttr);
    retval |= copyCommonVariableAttributes(node, &editAttr);
    UA_DataValue_deleteMembers(&node->value.data.value);
    node->valueSource = UA_VALUESOURCE_DATASOURCE;
    node->value.dataSource = dataSource;
    UA_DataValue_deleteMembers(&value);
    if(retval != UA_STATUSCODE_GOOD) {
        UA_NodeStore_deleteNode((UA_Node*)node);
        UA_RCU_UNLOCK();
        return retval;
    }

    /* Add the node */
    UA_AddNodesResult result;
    UA_AddNodesResult_init(&result);
    UA_RCU_LOCK();
    retval = UA_Server_addNode(server, &adminSession, (UA_Node*)node, &parentNodeId,
                               &referenceTypeId, &typeDefinition, NULL, outNewNodeId);
    UA_RCU_UNLOCK();
    return retval;
}

#ifdef UA_ENABLE_METHODCALLS

UA_StatusCode
UA_Server_addMethodNode(UA_Server *server, const UA_NodeId requestedNewNodeId,
                        const UA_NodeId parentNodeId, const UA_NodeId referenceTypeId,
                        const UA_QualifiedName browseName, const UA_MethodAttributes attr,
                        UA_MethodCallback method, void *handle,
                        size_t inputArgumentsSize, const UA_Argument* inputArguments,
                        size_t outputArgumentsSize, const UA_Argument* outputArguments,
                        UA_NodeId *outNewNodeId) {
    UA_MethodNode *node = UA_NodeStore_newMethodNode();
    if(!node)
        return UA_STATUSCODE_BADOUTOFMEMORY;

    UA_AddNodesItem item;
    UA_AddNodesItem_init(&item);
    item.requestedNewNodeId.nodeId = requestedNewNodeId;
    item.browseName = browseName;
    copyStandardAttributes((UA_Node*)node, &item, (const UA_NodeAttributes*)&attr);
    node->executable = attr.executable;
    node->attachedMethod = method;
    node->methodHandle = handle;

    /* Add the node */
    UA_NodeId newMethodId;
    UA_NodeId_init(&newMethodId);
    UA_RCU_LOCK();
    UA_StatusCode retval = UA_Server_addNode(server, &adminSession, (UA_Node*)node, &parentNodeId,
                                             &referenceTypeId, &UA_NODEID_NULL, NULL, &newMethodId);
    UA_RCU_UNLOCK();
    if(retval != UA_STATUSCODE_GOOD)
        return retval;

    const UA_NodeId hasproperty = UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY);
    const UA_NodeId propertytype = UA_NODEID_NUMERIC(0, UA_NS0ID_PROPERTYTYPE);

    if(inputArgumentsSize > 0) {
        UA_VariableNode *inputArgumentsVariableNode = UA_NodeStore_newVariableNode();
        inputArgumentsVariableNode->nodeId.namespaceIndex = newMethodId.namespaceIndex;
        inputArgumentsVariableNode->browseName = UA_QUALIFIEDNAME_ALLOC(0, "InputArguments");
        inputArgumentsVariableNode->displayName = UA_LOCALIZEDTEXT_ALLOC("en_US", "InputArguments");
        inputArgumentsVariableNode->description = UA_LOCALIZEDTEXT_ALLOC("en_US", "InputArguments");

        /* UAExpert creates a monitoreditem on inputarguments ... */
        inputArgumentsVariableNode->minimumSamplingInterval = 10000.0f;

        //TODO: 0.3 work item: the addMethodNode API does not have the possibility to set nodeIDs
        //actually we need to change the signature to pass UA_NS0ID_SERVER_GETMONITOREDITEMS_INPUTARGUMENTS
        //and UA_NS0ID_SERVER_GETMONITOREDITEMS_OUTPUTARGUMENTS into the function :/
        if(newMethodId.namespaceIndex == 0 &&
           newMethodId.identifierType == UA_NODEIDTYPE_NUMERIC &&
           newMethodId.identifier.numeric == UA_NS0ID_SERVER_GETMONITOREDITEMS) {
            inputArgumentsVariableNode->nodeId =
                UA_NODEID_NUMERIC(0, UA_NS0ID_SERVER_GETMONITOREDITEMS_INPUTARGUMENTS);
        }
        UA_Variant_setArrayCopy(&inputArgumentsVariableNode->value.data.value.value,
                                inputArguments, inputArgumentsSize,
                                &UA_TYPES[UA_TYPES_ARGUMENT]);
        inputArgumentsVariableNode->value.data.value.hasValue = true;

        inputArgumentsVariableNode->valueRank = 1;
        inputArgumentsVariableNode->dataType = UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATATYPE);

        UA_RCU_LOCK();
        // todo: check if adding succeeded
        UA_Server_addNode(server, &adminSession, (UA_Node*)inputArgumentsVariableNode,
                          &newMethodId, &hasproperty, &propertytype, NULL, NULL);
        UA_RCU_UNLOCK();
    }

    if(outputArgumentsSize > 0) {
        /* create OutputArguments */
        UA_VariableNode *outputArgumentsVariableNode  = UA_NodeStore_newVariableNode();
        outputArgumentsVariableNode->nodeId.namespaceIndex = newMethodId.namespaceIndex;
        outputArgumentsVariableNode->browseName  = UA_QUALIFIEDNAME_ALLOC(0, "OutputArguments");
        outputArgumentsVariableNode->displayName = UA_LOCALIZEDTEXT_ALLOC("en_US", "OutputArguments");
        outputArgumentsVariableNode->description = UA_LOCALIZEDTEXT_ALLOC("en_US", "OutputArguments");

        //FIXME: comment in line 882
        if(newMethodId.namespaceIndex == 0 &&
           newMethodId.identifierType == UA_NODEIDTYPE_NUMERIC &&
           newMethodId.identifier.numeric == UA_NS0ID_SERVER_GETMONITOREDITEMS) {
            outputArgumentsVariableNode->nodeId =
                UA_NODEID_NUMERIC(0, UA_NS0ID_SERVER_GETMONITOREDITEMS_OUTPUTARGUMENTS);
        }
        UA_Variant_setArrayCopy(&outputArgumentsVariableNode->value.data.value.value,
                                outputArguments, outputArgumentsSize,
                                &UA_TYPES[UA_TYPES_ARGUMENT]);
        outputArgumentsVariableNode->value.data.value.hasValue = true;

        outputArgumentsVariableNode->valueRank = 1;
        outputArgumentsVariableNode->dataType = UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATATYPE);

        UA_RCU_LOCK();
        // todo: check if adding succeeded
        UA_Server_addNode(server, &adminSession, (UA_Node*)outputArgumentsVariableNode,
                          &newMethodId, &hasproperty, &propertytype, NULL, NULL);
        UA_RCU_UNLOCK();
    }

    if(outNewNodeId)
        *outNewNodeId = newMethodId;
    else
        UA_NodeId_deleteMembers(&newMethodId);
    return retval;
}

#endif

/******************/
/* Add References */
/******************/

static UA_StatusCode
deleteOneWayReference(UA_Server *server, UA_Session *session, UA_Node *node,
                      const UA_DeleteReferencesItem *item);

/* Adds a one-way reference to the local nodestore */
static UA_StatusCode
addOneWayReference(UA_Server *server, UA_Session *session,
                   UA_Node *node, const UA_AddReferencesItem *item) {
    size_t i = node->referencesSize;
    size_t refssize = (i+1) | 3; // so the realloc is not necessary every time
    UA_ReferenceNode *new_refs = UA_realloc(node->references, sizeof(UA_ReferenceNode) * refssize);
    if(!new_refs)
        return UA_STATUSCODE_BADOUTOFMEMORY;
    node->references = new_refs;
    UA_ReferenceNode_init(&new_refs[i]);
    UA_StatusCode retval = UA_NodeId_copy(&item->referenceTypeId, &new_refs[i].referenceTypeId);
    retval |= UA_ExpandedNodeId_copy(&item->targetNodeId, &new_refs[i].targetId);
    new_refs[i].isInverse = !item->isForward;
    if(retval == UA_STATUSCODE_GOOD)
        node->referencesSize = i+1;
    else
        UA_ReferenceNode_deleteMembers(&new_refs[i]);
    return retval;
}

UA_StatusCode
Service_AddReferences_single(UA_Server *server, UA_Session *session,
                             const UA_AddReferencesItem *item) {
    /* Currently no expandednodeids are allowed */
    if(item->targetServerUri.length > 0)
        return UA_STATUSCODE_BADNOTIMPLEMENTED;

    /* Add the first direction */
#ifndef UA_ENABLE_EXTERNAL_NAMESPACES
    UA_StatusCode retval =
        UA_Server_editNode(server, session, &item->sourceNodeId,
                           (UA_EditNodeCallback)addOneWayReference, item);
#else
    UA_StatusCode retval = UA_STATUSCODE_GOOD;
    UA_Boolean handledExternally = UA_FALSE;
    for(size_t j = 0; j <server->externalNamespacesSize; ++j) {
        if(item->sourceNodeId.namespaceIndex != server->externalNamespaces[j].index) {
            continue;
        } else {
            UA_ExternalNodeStore *ens = &server->externalNamespaces[j].externalNodeStore;
            retval = (UA_StatusCode)ens->addOneWayReference(ens->ensHandle, item);
            handledExternally = UA_TRUE;
            break;
        }
    }
    if(!handledExternally)
        retval = UA_Server_editNode(server, session, &item->sourceNodeId,
                                    (UA_EditNodeCallback)addOneWayReference, item);
#endif

    if(retval != UA_STATUSCODE_GOOD)
        return retval;

    /* Add the second direction */
    UA_AddReferencesItem secondItem;
    UA_AddReferencesItem_init(&secondItem);
    secondItem.sourceNodeId = item->targetNodeId.nodeId;
    secondItem.referenceTypeId = item->referenceTypeId;
    secondItem.isForward = !item->isForward;
    secondItem.targetNodeId.nodeId = item->sourceNodeId;
    /* keep default secondItem.targetNodeClass = UA_NODECLASS_UNSPECIFIED */
#ifndef UA_ENABLE_EXTERNAL_NAMESPACES
    retval = UA_Server_editNode(server, session, &secondItem.sourceNodeId,
                                (UA_EditNodeCallback)addOneWayReference, &secondItem);
#else
    handledExternally = UA_FALSE;
    for(size_t j = 0; j < server->externalNamespacesSize; ++j) {
        if(secondItem.sourceNodeId.namespaceIndex != server->externalNamespaces[j].index) {
            continue;
        } else {
            UA_ExternalNodeStore *ens = &server->externalNamespaces[j].externalNodeStore;
            retval = (UA_StatusCode)ens->addOneWayReference(ens->ensHandle, &secondItem);
            handledExternally = UA_TRUE;
            break;
        }
    }
    if(!handledExternally)
        retval = UA_Server_editNode(server, session, &secondItem.sourceNodeId,
                                    (UA_EditNodeCallback)addOneWayReference, &secondItem);
#endif

    /* remove reference if the second direction failed */
    if(retval != UA_STATUSCODE_GOOD) {
        UA_DeleteReferencesItem deleteItem;
        deleteItem.sourceNodeId = item->sourceNodeId;
        deleteItem.referenceTypeId = item->referenceTypeId;
        deleteItem.isForward = item->isForward;
        deleteItem.targetNodeId = item->targetNodeId;
        deleteItem.deleteBidirectional = false;
        /* ignore returned status code */
        UA_Server_editNode(server, session, &item->sourceNodeId,
                           (UA_EditNodeCallback)deleteOneWayReference, &deleteItem);
    }
    return retval;
}

void Service_AddReferences(UA_Server *server, UA_Session *session,
                           const UA_AddReferencesRequest *request,
                           UA_AddReferencesResponse *response) {
    UA_LOG_DEBUG_SESSION(server->config.logger, session,
                         "Processing AddReferencesRequest");
    if(request->referencesToAddSize <= 0) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADNOTHINGTODO;
        return;
    }

    response->results = UA_malloc(sizeof(UA_StatusCode) * request->referencesToAddSize);
    if(!response->results) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADOUTOFMEMORY;
        return;
    }
    response->resultsSize = request->referencesToAddSize;

#ifndef UA_ENABLE_EXTERNAL_NAMESPACES
    for(size_t i = 0; i < response->resultsSize; ++i)
        response->results[i] =
            Service_AddReferences_single(server, session, &request->referencesToAdd[i]);
#else
    size_t size = request->referencesToAddSize;
# ifdef NO_ALLOCA
    UA_Boolean isExternal[size];
    UA_UInt32 indices[size];
# else
    UA_Boolean *isExternal = UA_alloca(sizeof(UA_Boolean) * size);
    UA_UInt32 *indices = UA_alloca(sizeof(UA_UInt32) * size);
# endif /*NO_ALLOCA */
    memset(isExternal, false, sizeof(UA_Boolean) * size);
    for(size_t j = 0; j < server->externalNamespacesSize; ++j) {
        size_t indicesSize = 0;
        for(size_t i = 0;i < size;++i) {
            if(request->referencesToAdd[i].sourceNodeId.namespaceIndex
               != server->externalNamespaces[j].index)
                continue;
            isExternal[i] = true;
            indices[indicesSize] = (UA_UInt32)i;
            ++indicesSize;
        }
        if (indicesSize == 0)
            continue;
        UA_ExternalNodeStore *ens = &server->externalNamespaces[j].externalNodeStore;
        ens->addReferences(ens->ensHandle, &request->requestHeader, request->referencesToAdd,
                           indices, (UA_UInt32)indicesSize, response->results, response->diagnosticInfos);
    }

    for(size_t i = 0; i < response->resultsSize; ++i) {
        if(!isExternal[i])
            response->results[i] =
                Service_AddReferences_single(server, session, &request->referencesToAdd[i]);
    }
#endif
}

UA_StatusCode
UA_Server_addReference(UA_Server *server, const UA_NodeId sourceId, const UA_NodeId refTypeId,
                       const UA_ExpandedNodeId targetId, UA_Boolean isForward) {
    UA_AddReferencesItem item;
    UA_AddReferencesItem_init(&item);
    item.sourceNodeId = sourceId;
    item.referenceTypeId = refTypeId;
    item.isForward = isForward;
    item.targetNodeId = targetId;
    UA_RCU_LOCK();
    UA_StatusCode retval = Service_AddReferences_single(server, &adminSession, &item);
    UA_RCU_UNLOCK();
    return retval;
}

/****************/
/* Delete Nodes */
/****************/

UA_StatusCode
Service_DeleteNodes_single(UA_Server *server, UA_Session *session,
                           const UA_NodeId *nodeId, UA_Boolean deleteReferences) {
    const UA_Node *node = UA_NodeStore_get(server->nodestore, nodeId);
    if(!node)
        return UA_STATUSCODE_BADNODEIDUNKNOWN;

    /* destroy an object before removing it */
    if(node->nodeClass == UA_NODECLASS_OBJECT) {
        /* find the object type(s) */
        UA_BrowseDescription bd;
        UA_BrowseDescription_init(&bd);
        bd.browseDirection = UA_BROWSEDIRECTION_INVERSE;
        bd.nodeId = *nodeId;
        bd.referenceTypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_HASSUBTYPE);
        bd.includeSubtypes = true;
        bd.nodeClassMask = UA_NODECLASS_OBJECTTYPE;

        /* browse type definitions with admin rights */
        UA_BrowseResult result;
        UA_BrowseResult_init(&result);
        Service_Browse_single(server, &adminSession, NULL, &bd, 0, &result);
        for(size_t i = 0; i < result.referencesSize; ++i) {
            /* call the destructor */
            UA_ReferenceDescription *rd = &result.references[i];
            const UA_ObjectTypeNode *typenode =
                (const UA_ObjectTypeNode*)UA_NodeStore_get(server->nodestore, &rd->nodeId.nodeId);
            if(!typenode)
                continue;
            if(typenode->nodeClass != UA_NODECLASS_OBJECTTYPE || !typenode->lifecycleManagement.destructor)
                continue;

            /* if there are several types with lifecycle management, call all the destructors */
            typenode->lifecycleManagement.destructor(*nodeId, ((const UA_ObjectNode*)node)->instanceHandle);
        }
        UA_BrowseResult_deleteMembers(&result);
    }

    /* remove references */
    /* TODO: check if consistency is violated */
    if(deleteReferences == true) { 
        for(size_t i = 0; i < node->referencesSize; ++i) {
            UA_DeleteReferencesItem item;
            UA_DeleteReferencesItem_init(&item);
            item.isForward = node->references[i].isInverse;
            item.sourceNodeId = node->references[i].targetId.nodeId;
            item.targetNodeId.nodeId = node->nodeId;
            UA_Server_editNode(server, session, &node->references[i].targetId.nodeId,
                               (UA_EditNodeCallback)deleteOneWayReference, &item);
        }
    }

    return UA_NodeStore_remove(server->nodestore, nodeId);
}

void Service_DeleteNodes(UA_Server *server, UA_Session *session,
                         const UA_DeleteNodesRequest *request,
                         UA_DeleteNodesResponse *response) {
    UA_LOG_DEBUG_SESSION(server->config.logger, session,
                         "Processing DeleteNodesRequest");
    if(request->nodesToDeleteSize == 0) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADNOTHINGTODO;
        return;
    }

    response->results = UA_malloc(sizeof(UA_StatusCode) * request->nodesToDeleteSize);
    if(!response->results) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADOUTOFMEMORY;;
        return;
    }
    response->resultsSize = request->nodesToDeleteSize;

    for(size_t i = 0; i < request->nodesToDeleteSize; ++i) {
        UA_DeleteNodesItem *item = &request->nodesToDelete[i];
        response->results[i] = Service_DeleteNodes_single(server, session, &item->nodeId,
                                                          item->deleteTargetReferences);
    }
}

UA_StatusCode
UA_Server_deleteNode(UA_Server *server, const UA_NodeId nodeId,
                     UA_Boolean deleteReferences) {
    UA_RCU_LOCK();
    UA_StatusCode retval = Service_DeleteNodes_single(server, &adminSession,
                                                      &nodeId, deleteReferences);
    UA_RCU_UNLOCK();
    return retval;
}

/*********************/
/* Delete References */
/*********************/

// TODO: Check consistency constraints, remove the references.
static UA_StatusCode
deleteOneWayReference(UA_Server *server, UA_Session *session, UA_Node *node,
                      const UA_DeleteReferencesItem *item) {
    UA_Boolean edited = false;
    for(size_t i = node->referencesSize-1; ; --i) {
        if(i > node->referencesSize)
            break; /* underflow after i == 0 */
        if(!UA_NodeId_equal(&item->targetNodeId.nodeId, &node->references[i].targetId.nodeId))
            continue;
        if(!UA_NodeId_equal(&item->referenceTypeId, &node->references[i].referenceTypeId))
            continue;
        if(item->isForward == node->references[i].isInverse)
            continue;
        /* move the last entry to override the current position */
        UA_ReferenceNode_deleteMembers(&node->references[i]);
        node->references[i] = node->references[node->referencesSize-1];
        --node->referencesSize;
        edited = true;
        break;
    }
    if(!edited)
        return UA_STATUSCODE_UNCERTAINREFERENCENOTDELETED;
    /* we removed the last reference */
    if(node->referencesSize == 0 && node->references) {
        UA_free(node->references);
        node->references = NULL;
    }
    return UA_STATUSCODE_GOOD;;
}

UA_StatusCode
Service_DeleteReferences_single(UA_Server *server, UA_Session *session,
                                const UA_DeleteReferencesItem *item) {
    UA_StatusCode retval = UA_Server_editNode(server, session, &item->sourceNodeId,
                                              (UA_EditNodeCallback)deleteOneWayReference, item);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;
    if(!item->deleteBidirectional || item->targetNodeId.serverIndex != 0)
        return retval;
    UA_DeleteReferencesItem secondItem;
    UA_DeleteReferencesItem_init(&secondItem);
    secondItem.isForward = !item->isForward;
    secondItem.sourceNodeId = item->targetNodeId.nodeId;
    secondItem.targetNodeId.nodeId = item->sourceNodeId;
    return UA_Server_editNode(server, session, &secondItem.sourceNodeId,
                              (UA_EditNodeCallback)deleteOneWayReference, &secondItem);
}

void
Service_DeleteReferences(UA_Server *server, UA_Session *session,
                         const UA_DeleteReferencesRequest *request,
                         UA_DeleteReferencesResponse *response) {
    UA_LOG_DEBUG_SESSION(server->config.logger, session,
                         "Processing DeleteReferencesRequest");
    if(request->referencesToDeleteSize <= 0) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADNOTHINGTODO;
        return;
    }

    response->results = UA_malloc(sizeof(UA_StatusCode) * request->referencesToDeleteSize);
    if(!response->results) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADOUTOFMEMORY;;
        return;
    }
    response->resultsSize = request->referencesToDeleteSize;

    for(size_t i = 0; i < request->referencesToDeleteSize; ++i)
        response->results[i] =
            Service_DeleteReferences_single(server, session, &request->referencesToDelete[i]);
}

UA_StatusCode
UA_Server_deleteReference(UA_Server *server, const UA_NodeId sourceNodeId,
                          const UA_NodeId referenceTypeId, UA_Boolean isForward,
                          const UA_ExpandedNodeId targetNodeId, UA_Boolean deleteBidirectional) {
    UA_DeleteReferencesItem item;
    item.sourceNodeId = sourceNodeId;
    item.referenceTypeId = referenceTypeId;
    item.isForward = isForward;
    item.targetNodeId = targetNodeId;
    item.deleteBidirectional = deleteBidirectional;
    UA_RCU_LOCK();
    UA_StatusCode retval = Service_DeleteReferences_single(server, &adminSession, &item);
    UA_RCU_UNLOCK();
    return retval;
}

/**********************/
/* Set Value Callback */
/**********************/

static UA_StatusCode
setValueCallback(UA_Server *server, UA_Session *session,
                 UA_VariableNode *node, UA_ValueCallback *callback) {
    if(node->nodeClass != UA_NODECLASS_VARIABLE)
        return UA_STATUSCODE_BADNODECLASSINVALID;
    node->value.data.callback = *callback;
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode UA_EXPORT
UA_Server_setVariableNode_valueCallback(UA_Server *server, const UA_NodeId nodeId,
                                        const UA_ValueCallback callback) {
    UA_RCU_LOCK();
    UA_StatusCode retval = UA_Server_editNode(server, &adminSession, &nodeId,
                                              (UA_EditNodeCallback)setValueCallback, &callback);
    UA_RCU_UNLOCK();
    return retval;
}

/******************/
/* Set DataSource */
/******************/

static UA_StatusCode
setDataSource(UA_Server *server, UA_Session *session,
              UA_VariableNode* node, UA_DataSource *dataSource) {
    if(node->nodeClass != UA_NODECLASS_VARIABLE)
        return UA_STATUSCODE_BADNODECLASSINVALID;
    if(node->valueSource == UA_VALUESOURCE_DATA)
        UA_DataValue_deleteMembers(&node->value.data.value);
    node->value.dataSource = *dataSource;
    node->valueSource = UA_VALUESOURCE_DATASOURCE;
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode
UA_Server_setVariableNode_dataSource(UA_Server *server, const UA_NodeId nodeId,
                                     const UA_DataSource dataSource) {
    UA_RCU_LOCK();
    UA_StatusCode retval = UA_Server_editNode(server, &adminSession, &nodeId,
                                              (UA_EditNodeCallback)setDataSource, &dataSource);
    UA_RCU_UNLOCK();
    return retval;
}

/****************************/
/* Set Lifecycle Management */
/****************************/

static UA_StatusCode
setOLM(UA_Server *server, UA_Session *session,
       UA_ObjectTypeNode* node, UA_ObjectLifecycleManagement *olm) {
    if(node->nodeClass != UA_NODECLASS_OBJECTTYPE)
        return UA_STATUSCODE_BADNODECLASSINVALID;
    node->lifecycleManagement = *olm;
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode UA_EXPORT
UA_Server_setObjectTypeNode_lifecycleManagement(UA_Server *server, UA_NodeId nodeId,
                                                UA_ObjectLifecycleManagement olm) {
    UA_RCU_LOCK();
    UA_StatusCode retval = UA_Server_editNode(server, &adminSession, &nodeId,
                                              (UA_EditNodeCallback)setOLM, &olm);
    UA_RCU_UNLOCK();
    return retval;
}

/***********************/
/* Set Method Callback */
/***********************/

#ifdef UA_ENABLE_METHODCALLS

struct addMethodCallback {
    UA_MethodCallback callback;
    void *handle;
};

static UA_StatusCode
editMethodCallback(UA_Server *server, UA_Session* session,
                   UA_Node* node, const void* handle) {
    if(node->nodeClass != UA_NODECLASS_METHOD)
        return UA_STATUSCODE_BADNODECLASSINVALID;
    const struct addMethodCallback *newCallback = handle;
    UA_MethodNode *mnode = (UA_MethodNode*) node;
    mnode->attachedMethod = newCallback->callback;
    mnode->methodHandle   = newCallback->handle;
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode UA_EXPORT
UA_Server_setMethodNode_callback(UA_Server *server, const UA_NodeId methodNodeId,
                                 UA_MethodCallback method, void *handle) {
    struct addMethodCallback cb = { method, handle };
    UA_RCU_LOCK();
    UA_StatusCode retval = UA_Server_editNode(server, &adminSession,
                                              &methodNodeId, editMethodCallback, &cb);
    UA_RCU_UNLOCK();
    return retval;
}

#endif
