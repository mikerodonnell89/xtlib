/*
 * This file is part of the xTuple ERP: PostBooks Edition, a free and
 * open source Enterprise Resource Planning software suite,
 * Copyright (c) 1999-2011 by OpenMFG LLC, d/b/a xTuple.
 * It is licensed to you under the Common Public Attribution License
 * version 1.0, the full text of which (including xTuple-specific Exhibits)
 * is available at www.xtuple.com/CPAL.  By using this software, you agree
 * to be bound by its terms.
 */

// TODO: does type really belong as a property of all xtStorables?

/** \class xtStorable
  
  \brief Parent class for all objects that can be made persistent.
  
  All storable objects have an internal id. If the object has not yet been
  saved to the storage medium, its id has the value 0.
  Storable objects also have the following properties:
  - \c creator - the user who first stored this object.
  - \c created - the date and time this object was first stored.
  - \c modifier - the user who last changed this object in the data store.
  - \c modified - the date and time this object was last changed in the data store.
  
  All storable objects have the following internal states and transitions:
   \code
                                                 saveAs()
       setDeleted()                              save()
       reload()                                  reload()
    +--------------------------+          +------------------------+    
    |                          |          |                        |    
    |                          |          |                        |    
    |  setDeleted()            |          |                        |    
    |  reload()      setX()-   |          |  reload()    setX()-   |    
    | +---------+  +---------+ | saveAs() | +--------+ +-----------+    
    v v         |  v         | | save()   v v        | v           |    
  -> N --------> IC --------> V ---------> P ------> PIC --------> PV
    ^ | setX()      setX()*   ^         7 | | setX()     setX()*   ^
    | |                       |        /  | |                      |
    | +-----------------------+       /   | +----------------------+ 
    |         setX()*                +----+          setX()
    |                         setDeleted()|                       
    +-------------------------------------+
  \endcode

  <table>
    <tr>
      <td> N </td> <td> Null </td>
      <td> this is a place-holder state because we have to start somewhere</td>
    </tr>
    <tr>
      <td> IC </td> <td> Incomplete and not yet persistent </td>
      <td> some data have been entered
        but the object is missing at least one required datum </td>
      </tr>
    <tr>
      <td> V </td> <td>  Valid and not yet persistent </td>
      <td> all required data have been entered </td>
    </tr>
    <tr>
      <td> P </td> <td>  Persistent </td>
      <td> the data have been recorded </td>
    </tr>
    <tr>
      <td> PIC</td> <td> Incomplete and persistent </td>
      <td> a persistent object has been opened
       for editing but the object is missing at least one required datum </td>
      </tr>
    <tr>
      <td> PV</td> <td>  Valid and not yet persistent </td>
      <td> all required data have been entered </td>
    </tr>
  </table>

  Notes:
  <ul>
  <li> \c setX()*  the call to a set() method that sets a value for the last
             required datum in this storable object
  </li>
  <li> \c setX()-  the call to a set() method that unsets a required datum
             or sets a value that increases the number of required data
             (e.g. if selecting an option to ship an order instead of
             having the customer pick up the order, the ship-to address
             becomes required where previously it wasn't).
  </li>

  <li> More complex storable objects may have more complex transition
     diagrams.  The expectation is that any additional complexity will
     leave this portion alone and add one or more transitions out of
     state P. Any state that allows editing must have xIC and xV states
     that follow the same pattern established here.
  </li>

  <li> Being in the IC state does not imply that no data have been
     written to the database. For example:
     <ul>
     <li>to avoid allocating the same number to multiple sales orders,
       the transition N -> IC might record that a particular sales
       order number has been allocated.
     </li>

     <li> Transitions between N and IC or V might write lock information,
       as might transitions between P and PIC or PV
     </li>
     </ul>
  </li>

  <li> Not all of these states need to be visible to the outside
     world. For example, the outside world might see IC, V, PIC,
     and PV as "locked".
  </li>

  <li> Different subclasses might need different styles of delete,
     hard and soft, hence the two arrows marked setDeleted() leaving \c P.
     For hard
     deletes, setDeleted() would take an item from state P to N. Soft
     deletes would take an item from state P back to P, but with
     changed data.
  </li>
  </ul>

*/

#include "xtStorable.h"

#include "xtAnyUtility.h"
#include "xtFieldData.h"
#include "interfaces/xiPropertyChecker.h"
#include "xtQuery.h"
#include "exceptions/xeInvalidStorable.h"
#include "exceptions/xeDataNotFound.h"
#include "xtSecurity.h"

#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

#include <boost/any.hpp>
#include <boost/lexical_cast.hpp>
#include "boost/date_time/posix_time/posix_time.hpp"

using namespace boost;

//
// xtStorablePrivate implementation
//
class xtStorablePrivate
{
  public:
    xtStorablePrivate(xtStorable *);
    virtual ~xtStorablePrivate();

    xtStorable * _owner;

    long long _id;
    std::string _creator;
    std::string _modifier;

    std::string _type;
    std::string _tableName;
    std::string _prefix;
    bool _locked;
    bool _deleted;

    bool _enforceReadOnly;
    class ReadOnlyChecker : public xiPropertyChecker
    {
      xtStorablePrivate * self;
      public:
        ReadOnlyChecker(xtStorablePrivate * parent) : self(parent) {}
        bool check(boost::any, int role)
        {
          if(self->_enforceReadOnly && role == xtlib::ValueRole)
            return false;
          return true;
        }
    };
    xiPropertyChecker * readOnly;
};

xtStorablePrivate::xtStorablePrivate(xtStorable * owner)
{
  _id    = 0;
  _owner = owner;
  _locked = false;
  _deleted = false;
  _enforceReadOnly = true;
  readOnly = new ReadOnlyChecker(this);
}

xtStorablePrivate::~xtStorablePrivate()
{
}
 
//
// xtStorable Implementation
//

/**
    \brief Construct a default xtStorable object.
 */
xtStorable::xtStorable()
  : xtObject()
{
  _data = new xtStorablePrivate(this);
  setPropertyP("creator", _data->readOnly, xtlib::CheckerRole);
  setPropertyP("created", _data->readOnly, xtlib::CheckerRole);
  setPropertyP("modifier", _data->readOnly, xtlib::CheckerRole);
  setPropertyP("modified", _data->readOnly, xtlib::CheckerRole);
  setPropertyP("type", _data->readOnly, xtlib::CheckerRole);

  setPropertyP("creator", xtFieldData("creator", xtFieldData::String), xtlib::FieldRole);
  setPropertyP("created", xtFieldData("created", xtFieldData::Timestamp), xtlib::FieldRole);
  setPropertyP("modifier", xtFieldData("modifier", xtFieldData::String), xtlib::FieldRole);
  setPropertyP("modified", xtFieldData("modified", xtFieldData::Timestamp), xtlib::FieldRole);
  setPropertyP("type", xtFieldData("type", xtFieldData::String), xtlib::FieldRole);
}

/**
    \brief Destruct an \c xtStorable object.
 */
xtStorable::~xtStorable() 
{
  if(isSelfLocked())
    unlock();

  if(_data)
    delete _data;
}

/**
    \brief Loads the object with the specified ID from the data store.

    \param id The ID of the object to load.
    \throw std::runtime_error If no table name was specified
                              a \c std::runtime_error will be thrown.
 */
void xtStorable::load(long long id)
{
  if(_data->_tableName.empty())
    throw std::runtime_error("cannot load from the database without a table name");

  std::string sql = "SELECT * FROM \"";
  sql += _data->_tableName;
  sql += "\" WHERE ";
  sql += _data->_prefix;
  sql += "id = '";
  sql += boost::lexical_cast<std::string>(id);
  sql += "';";

  if(xtlib::debug)
    std::cerr << "executing: " << sql << std::endl;
  xtQuery query;
  query.exec(sql);

  if(query.rowCount() < 1)
    return;

  _data->_enforceReadOnly = false;
  std::set<std::string> propList = getPropertyNames(xtlib::FieldRole);
  for (std::set<std::string>::const_iterator it = propList.begin();
       it != propList.end();
       it++)
  {
    boost::any pFieldData = getProperty(*it, xtlib::FieldRole);
    try
    {
      xtFieldData fd = boost::any_cast<xtFieldData>(pFieldData);
      boost::any val; // an empty value which we'll leave for null values
      std::string fieldName = _data->_prefix + fd.fieldName;
      if(!query.isNull(0, fieldName))
      {
        std::string sval = query.getValue(0, fieldName); // get the value which is a string? 
        switch(fd.type)
        {
          case xtFieldData::Serial:
            val = lexical_cast<long long>(sval);
            break;
          case xtFieldData::Integer:
            val = lexical_cast<int>(sval);
            break;
          case xtFieldData::Numeric:
            val = lexical_cast<double>(sval);
            break;
          case xtFieldData::Bool:
            val = lexical_cast<bool>(sval);
            break;
          case xtFieldData::Timestamp:
          case xtFieldData::TimestampTZ:
            val = boost::posix_time::time_from_string(sval); // TODO: are these really the same?
            break;
          case xtFieldData::Date:
            val = boost::gregorian::from_string(sval);
            break;
          case xtFieldData::Time:
            // TODO: figure this one out
          case xtFieldData::String:
          default:
            val = sval;
        };
      }
      
      setProperty(*it, val);
      setPropertyP(*it, val, xtlib::PreviousValueRole);
    }
    catch(const boost::bad_any_cast &e)
    {
      std::cerr << "problem reading field: " << e.what();
    }
  }
  _data->_enforceReadOnly = true;
  _data->_id = id;
}

/**
    \brief Reloads the objects properties from the storage system.
 */
void xtStorable::reload()
{
  // TODO: Do we need to do anything special? perhaps just use PreviousValueRole
  load(_data->_id);
}

/**
    \brief Saves the object to the storage system.

    If the object is new then a new record will be inserted into the
    storage system. Existing objects will be updated if any of the storable
    properties were changed.

    \throw xeInvalidStorable If the object is not valid an
                             xeInvalidStorable will be thrown.
 */
void xtStorable::save()
{
  if(!isValid())
    throw xeInvalidStorable(*this);

  if(isLocked() && !isSelfLocked())
    throw std::runtime_error("cannot save this object as it has been locked by someone else.");

  if(isDeleted())
  {
    doDelete();
    return;
  }

  std::map<std::string, boost::any> changed;
  std::set<std::string> fieldnames = getPropertyNames(xtlib::FieldRole);
  for (std::set<std::string>::const_iterator it = fieldnames.begin();
       it != fieldnames.end();
       it++)
  {
    if(! xtAnyUtility::equal(getProperty(*it),
                             getProperty(*it, xtlib::PreviousValueRole)))
      changed.insert(std::pair<std::string, boost::any>(*it, getProperty(*it)));
  }

  if(changed.size())
  {
    _data->_enforceReadOnly = false;
    std::string tuser = xtSecurity::logicalUser();
    setProperty("modifier", tuser);
    changed.insert(std::pair<std::string, boost::any>("modifier", tuser));
    boost::posix_time::ptime newt = boost::posix_time::second_clock::local_time();
    setProperty("modified", newt);
    changed.insert(std::pair<std::string, boost::any>("modified", newt));
    _data->_enforceReadOnly = true;
    std::string sql;
    if(_data->_id)
    {
      sql = "UPDATE \"";
      sql += _data->_tableName;
      sql += "\" SET ";
      int colcnt = 0;
      for (std::map<std::string, boost::any>::iterator it = changed.begin();
           it != changed.end();
           it++, colcnt++)
      {
        boost::any pFieldData = getProperty((*it).first, xtlib::FieldRole);
        xtFieldData fd = boost::any_cast<xtFieldData>(pFieldData);
        std::string fieldName = _data->_prefix + fd.fieldName;
        if(colcnt)
          sql += ", ";
        sql += "\"" + fieldName;
        sql += "\"='" + xtAnyUtility::toString((*it).second) + "'";
      }
      sql += " WHERE (" + _data->_prefix + "id='";
      sql += boost::lexical_cast<std::string>(_data->_id);
      sql += "');";
    }
    else
    {
      _data->_enforceReadOnly = false;
      setProperty("creator", tuser);
      changed.insert(std::pair<std::string, boost::any>("creator", tuser));
      setProperty("created", newt);
      changed.insert(std::pair<std::string, boost::any>("created", newt));
      _data->_enforceReadOnly = true;
      sql = "INSERT INTO \"";
      sql += _data->_tableName;
      sql += "\" (";

      int colcnt = 0;
      std::string values;

      for (std::map<std::string, boost::any>::iterator it = changed.begin();
           it != changed.end();
           it++, colcnt++)
      {
        boost::any pFieldData = getProperty((*it).first, xtlib::FieldRole);
        xtFieldData fd = boost::any_cast<xtFieldData>(pFieldData);
        std::string fieldName = _data->_prefix + fd.fieldName;
        if(colcnt)
        {
          sql    += ", ";
          values += ", ";
        }
        sql     += "\"" + fieldName  + "\"";
        values  += "'"  + xtAnyUtility::toString((*it).second) + "'";
      }
      sql += ") VALUES (";
      sql += values;
      sql += ") RETURNING " + _data->_prefix + "id;";
    }

    if(xtlib::debug)
      std::cerr << "executing: " << sql;
    xtQuery query;
    query.exec(sql);
    if(query.rowCount() >= 1)
      _data->_id = boost::lexical_cast<long long>(query.getValue(0, _data->_prefix + "id"));

    load(_data->_id);
  }
}

/**
    \brief Saves this object as a new object on the system.

    Calling this will change the internal state of the object so that
    it will represent an unassociated copy of the original object.
    The object will be saved the storage system and get a new internal
    ID.

    \throw xeInvalidStorable
 */
void xtStorable::saveAs()
{
  if (! isValid())
    throw xeInvalidStorable(*this);

  _data->_id = 0;

  std::set<std::string> fieldnames = getPropertyNames(xtlib::FieldRole);
  for (std::set<std::string>::const_iterator it = fieldnames.begin();
       it != fieldnames.end();
       it++)
  {
    setPropertyP(*it, boost::any(), xtlib::PreviousValueRole);
  }

  save();
}

/**
    \brief Will lock the record if it isn't already locked.
 */
void xtStorable::lock()
{
  if(_data->_tableName.empty())
    throw std::runtime_error("cannot lock the the object without a table name");

  if(isLocked())
    throw std::runtime_error("cannot lock an object that is already locked.");

  xtQuery qry;
  std::string sql = "SELECT pg_try_advisory_lock(oid::integer, ";
  sql += boost::lexical_cast<std::string>(_data->_id);
  sql += ") AS locked FROM pg_class WHERE relname='";
  sql += _data->_tableName;
  sql += "';";
  qry.exec(sql);
  if (qry.rowCount() > 0)
  {
    std::string sval = qry.getValue(0, "locked"); // get the value which is a string?
    bool val = sval == "t";
    if (val != true)
      throw std::runtime_error("cannot lock record that is already locked.");
    else
      _data->_locked = true;
  }
  else
    throw std::runtime_error("cannot lock record due to unexpected error.");
}

/**
    \brief Will unlock the record.

    A record can only unlock itself if it was the one that started the lock.
 */
void xtStorable::unlock()
{
  if(_data->_tableName.empty())
    throw std::runtime_error("cannot unlock the the object without a table name");

  if(!_data->_locked)
    throw std::runtime_error("cannot unlock the object when this instance didn't initiate the lock.");

  xtQuery qry;
  std::string sql = "SELECT pg_advisory_unlock(oid::integer, ";
  sql += boost::lexical_cast<std::string>(_data->_id);
  sql += ") AS result FROM pg_class WHERE relname='";
  sql += _data->_tableName;
  sql += "';";
  qry.exec(sql);
  if (qry.rowCount() > 0)
  {
    std::string sval = qry.getValue(0, "result"); // get the value which is a string?
    bool val = sval == "t";
    if (val != true)
      throw std::runtime_error("cannot unlock record due to an unexpected error.");
    else
      _data->_locked = false;
  }
  else
    throw std::runtime_error("cannot unlock record due to unexpected error.");
}

/**
    \brief Denotes if this object is locked.

    Returns true if this object is locked by anyone in the system,
    including if it locked itself.

    \return \c true if the object is locked.
 */
bool xtStorable::isLocked() const
{
  if(_data->_locked)
    return true; // we locked ourselves so we know we are locked

  // TODO: check to see if this object is locked by anyone.
  xtQuery qry;
  std::string sql = "SELECT pid FROM pg_locks WHERE ((classid=(SELECT oid::integer FROM pg_class WHERE relname='";
  sql += _data->_tableName;
  sql += "')) AND (objid=";
  sql += boost::lexical_cast<std::string>(_data->_id);
  sql += ") AND (objsubid=2));";
  qry.exec(sql);
  if(qry.rowCount() > 0)
    return true;

  return false;
}

/**
    \brief Denotes if this object locked itself.

    When the object is locked this will return true
    if the object locked itself.

    \return \c true if the object locked itself.
 */
bool xtStorable::isSelfLocked() const
{
  return _data->_locked;
}

/**
    \brief Checks if any xtlib::ValueRole properties have changed since
           this object was last saved or loaded.

    This will return \c true if any of the property values have changed
    since the object was loaded from or saved to the storage system.

    \return \c true if the object has changed.
 */
bool xtStorable::isDirty() const
{
  std::set<std::string> propList = getPropertyNames(xtlib::ValueRole);
  for (std::set<std::string>::const_iterator it = propList.begin();
       it != propList.end();
       it++)
  {
    if (! xtAnyUtility::equal(getProperty(*it),
                              getProperty(*it, xtlib::PreviousValueRole)))
      return true;
  }

  return false;
}

/**
    \brief Checks the validity of the object.

    \return \c true if the object is valid.
 */
bool xtStorable::isValid() const
{
  bool returnValue = true;

  if(_data->_tableName.empty())
    returnValue = false;
  else
  {
    std::set<std::string> names = getPropertyNames(xtlib::RequiredRole);

    for (std::set<std::string>::const_iterator it = names.begin();
         it != names.end();
         it++)
    {
      boost::any required = getProperty(*it, xtlib::RequiredRole);
      if (! required.empty() && any_cast<bool>(required))
        returnValue &= ! getProperty(*it).empty();
    }
  }

  return returnValue;
}

xtError xtStorable::getLastError() const
{
  // TODO: make this do something
  return xtError::NoError;
}

/**
    \brief Returns the storage system ID of this object.

    If the object has not been saved to the storage system then the id
    returned is 0.

    \return The ID of this object in the storage system.
 */
long long xtStorable::getId() const
{
  return _data->_id;
}

/**
    \brief Convenience method that returns the role value for the type property.

    \param role The role value to retreive.
    \return \c boost::any value or and empty \c boost::any.

    \sa xtObject::getProperty()
 */
boost::any xtStorable::getType(int role) const
{
  return getProperty("type", role);
}

/**
    \brief Convenience method that returns the role value for the created property.

    \param role The role value to retreive.
    \return \c boost::any value or and empty \c boost::any.

    \sa xtObject::getProperty()
 */
boost::any xtStorable::getCreated(int role) const
{
  return getProperty("created", role);
}

/**
    \brief Convenience method that returns the role value for the creator property.

    \param role The role value to retreive.
    \return \c boost::any value or and empty \c boost::any.

    \sa xtObject::getProperty()
 */
boost::any xtStorable::getCreator(int role) const
{
  return getProperty("creator", role);
}

/**
    \brief Convenience method that returns the role value for the modified property.

    \param role The role value to retreive.
    \return \c boost::any value or and empty \c boost::any.

    \sa xtObject::getProperty()
 */
boost::any xtStorable::getModified(int role) const
{
  return getProperty("modified", role);
}

/**
    \brief Convenience method that returns the role value for the modifier property.

    \param role The role value to retreive.
    \return \c boost::any value or and empty \c boost::any.

    \sa xtObject::getProperty()
 */
boost::any xtStorable::getModifier(int role) const
{
  return getProperty("modifier", role);
}

bool xtStorable::isDeleted() const
{
  return _data->_deleted;
}

void xtStorable::setDeleted(bool deleted)
{
  _data->_deleted = deleted; // the actual delete is handled later
}

void xtStorable::doDelete()
{
  // TODO: add functionality to support a soft-delete.
  // This function provides a basic delete feature that
  // can be overridden by sub-classes should they choose
  // to implement something different
  if(_data->_tableName.empty())
    throw std::runtime_error("cannot delete from the database without a table name");

  std::string sql = "DELETE FROM \"";
  sql += _data->_tableName;
  sql += "\" WHERE ";
  sql += _data->_prefix;
  sql += "id = '";
  sql += boost::lexical_cast<std::string>(getId());
  sql += "';";

  if(xtlib::debug)
    std::cerr << "executing: " << sql << std::endl;
  xtQuery query;
  query.exec(sql);
}

/**
    \brief Sets the type of the storable object.

    Can be used by subclasses to distinguish between different
    objects that are functionally the same but may be grouped
    differently or user defined.

    \param type The string representing this type.
 */
void xtStorable::setType(const std::string & type)
{
  _data->_enforceReadOnly = false;
  setProperty("type", type);
  _data->_enforceReadOnly = true;
}

/**
    \brief Sets the name of the table used by the storage system.

    Subclasses need to set this to an appropriate value before
    data can be sent to or retrieved from the storage system.

    \param name The name of the table to use.
 */
void xtStorable::setTableName(const std::string & name)
{
  _data->_tableName = name;
}

std::string xtStorable::getTableName() const
{
  return _data->_tableName;
}

/**
    \brief Sets the prefix of the fields used by the storage system.

    Subclasses need to set this to an appropriate value before
    data can be sent to or retrieved from the storage system.

    \param name The prefix of the fields to use.
 */
void xtStorable::setFieldPrefix(const std::string & name)
{
  _data->_prefix = name;
}

std::string xtStorable::getFieldPrefix() const
{
  return _data->_prefix;
}

