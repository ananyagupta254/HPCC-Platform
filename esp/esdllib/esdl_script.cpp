/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2020 HPCC Systems®.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
############################################################################## */

#include "espcontext.hpp"
#include "esdl_script.hpp"
#include "wsexcept.hpp"
#include "httpclient.hpp"
#include "dllserver.hpp"
#include "thorplugin.hpp"
#include "eclrtl.hpp"
#include "rtlformat.hpp"
#include "jsecrets.hpp"
#include "esdl_script.hpp"

#include <fxpp/FragmentedXmlPullParser.hpp>
using namespace xpp;

class OptionalCriticalBlock
{
    CriticalSection *crit = nullptr;
public:
    inline OptionalCriticalBlock(CriticalSection *c) : crit(c)
    {
        if (crit)
            crit->enter();
    }
    inline ~OptionalCriticalBlock()
    {
        if (crit)
            crit->leave();
    }
};

IEsdlTransformOperation *createEsdlTransformOperation(IXmlPullParser &xpp, const StringBuffer &prefix, bool withVariables, bool ignoreCodingErrors, IEsdlFunctionRegister *functionRegister, bool canCreateFunctions);
void createEsdlTransformOperations(IArrayOf<IEsdlTransformOperation> &operations, IXmlPullParser &xpp, const StringBuffer &prefix, bool withVariables, bool ignoreCodingErrors, IEsdlFunctionRegister *functionRegister);
void createEsdlTransformChooseOperations(IArrayOf<IEsdlTransformOperation> &operations, IXmlPullParser &xpp, const StringBuffer &prefix, bool withVariables, bool ignoreCodingErrors, IEsdlFunctionRegister *functionRegister);
typedef void (*esdlOperationsFactory_t)(IArrayOf<IEsdlTransformOperation> &operations, IXmlPullParser &xpp, const StringBuffer &prefix, bool withVariables, bool ignoreCodingErrors, IEsdlFunctionRegister *functionRegister);

bool getStartTagValueBool(StartTag &stag, const char *name, bool defaultValue)
{
    if (isEmptyString(name))
        return defaultValue;
    const char *value = stag.getValue(name);
    if (isEmptyString(value))
        return defaultValue;
    return strToBool(value);
}


inline void buildEsdlOperationMessage(StringBuffer &s, int code, const char *op, const char *msg, const char *traceName)
{
    s.set("ESDL Script: ");
    if (!isEmptyString(traceName))
        s.append(" '").append(traceName).append("' ");
    if (!isEmptyString(op))
        s.append(" ").append(op).append(" ");
    s.append(msg);
}

inline void esdlOperationWarning(int code, const char *op, const char *msg, const char *traceName)
{
    StringBuffer s;
    buildEsdlOperationMessage(s, code, op, msg, traceName);
    IWARNLOG("%s", s.str());
}

inline void esdlOperationError(int code, const char *op, const char *msg, const char *traceName, bool exception)
{
    StringBuffer s;
    buildEsdlOperationMessage(s, code, op, msg, traceName);
    IERRLOG("%s", s.str());
    if(exception)
        throw MakeStringException(code, "%s", s.str());
}

inline void esdlOperationError(int code, const char *op, const char *msg, bool exception)
{
    esdlOperationError(code, op, msg, "", exception);
}

static inline const char *checkSkipOpPrefix(const char *op, const StringBuffer &prefix)
{
    if (prefix.length())
    {
        if (!hasPrefix(op, prefix, true))
        {
            DBGLOG(1,"Unrecognized script operation: %s", op);
            return nullptr;
        }
        return (op + prefix.length());
    }
    return op;
}

static inline StringBuffer &makeOperationTagName(StringBuffer &s, const StringBuffer &prefix, const char *op)
{
    return s.append(prefix).append(op);
}

class CEsdlTransformOperationBase : public CInterfaceOf<IEsdlTransformOperation>
{
protected:
    StringAttr m_tagname;
    StringAttr m_traceName;
    bool m_ignoreCodingErrors = false; //ideally used only for debugging

public:
    CEsdlTransformOperationBase(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix)
    {
        m_tagname.set(stag.getLocalName());
        m_traceName.set(stag.getValue("trace"));
        m_ignoreCodingErrors = getStartTagValueBool(stag, "optional", false);
    }
    virtual bool process(IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext) override
    {
        return exec(nullptr, nullptr, scriptContext, targetContext, sourceContext);
    }
    virtual IInterface *prepareForAsync(IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext) override
    {
        return nullptr;
    }
};

class CEsdlTransformOperationWithChildren : public CEsdlTransformOperationBase
{
protected:
    IArrayOf<IEsdlTransformOperation> m_children;
    bool m_withVariables = false;
    XpathVariableScopeType m_childScopeType = XpathVariableScopeType::simple;

public:
    CEsdlTransformOperationWithChildren(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix, bool withVariables, IEsdlFunctionRegister *functionRegister, esdlOperationsFactory_t factory) : CEsdlTransformOperationBase(xpp, stag, prefix), m_withVariables(withVariables)
    {
        //load children
        if (factory)
            factory(m_children, xpp, prefix, withVariables, m_ignoreCodingErrors, functionRegister);
        else
            createEsdlTransformOperations(m_children, xpp, prefix, withVariables, m_ignoreCodingErrors, functionRegister);
    }

    virtual ~CEsdlTransformOperationWithChildren(){}

    virtual bool processChildren(IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext)
    {
        if (!m_children.length())
            return false;

        Owned<CXpathContextScope> scope = m_withVariables ? new CXpathContextScope(sourceContext, m_tagname, m_childScopeType, nullptr) : nullptr;
        bool ret = false;
        ForEachItemIn(i, m_children)
        {
            if (m_children.item(i).process(scriptContext, targetContext, sourceContext))
                ret = true;
        }
        return ret;
    }

    virtual void toDBGLog () override
    {
    #if defined(_DEBUG)
        ForEachItemIn(i, m_children)
            m_children.item(i).toDBGLog();
    #endif
    }
};

class CEsdlTransformOperationWithoutChildren : public CEsdlTransformOperationBase
{
public:
    CEsdlTransformOperationWithoutChildren(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix) : CEsdlTransformOperationBase(xpp, stag, prefix)
    {
        if (xpp.skipSubTreeEx())
            esdlOperationError(ESDL_SCRIPT_Error, m_tagname, "should not have child tags", m_traceName, !m_ignoreCodingErrors);
    }

    virtual ~CEsdlTransformOperationWithoutChildren(){}
};

class CEsdlTransformOperationVariable : public CEsdlTransformOperationWithChildren
{
protected:
    StringAttr m_name;
    Owned<ICompiledXpath> m_select;

public:
    CEsdlTransformOperationVariable(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix, IEsdlFunctionRegister *functionRegister)
        : CEsdlTransformOperationWithChildren(xpp, stag, prefix, true, functionRegister, nullptr)
    {
        if (m_traceName.isEmpty())
            m_traceName.set(stag.getValue("name"));
        m_name.set(stag.getValue("name"));
        if (m_name.isEmpty())
            esdlOperationError(ESDL_SCRIPT_MissingOperationAttr, m_tagname, "without name", m_traceName, !m_ignoreCodingErrors);
        const char *select = stag.getValue("select");
        if (!isEmptyString(select))
            m_select.setown(compileXpath(select));
    }

    virtual ~CEsdlTransformOperationVariable()
    {
    }

    virtual bool exec(CriticalSection *crit, IInterface *preparedForAsync, IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext) override
    {
        OptionalCriticalBlock block(crit);

        if (m_select)
            return sourceContext->addCompiledVariable(m_name, m_select);
        else if (m_children.length())
        {
            VStringBuffer xpath("/esdl_script_context/temporaries/%s", m_name.str());
            CXpathContextLocation location(targetContext);

            targetContext->ensureLocation(xpath, true);
            processChildren(scriptContext, targetContext, sourceContext); //bulid nodeset
            sourceContext->addXpathVariable(m_name, xpath);
            return false;
        }
        sourceContext->addVariable(m_name, "");
        return false;
    }

    virtual void toDBGLog() override
    {
#if defined(_DEBUG)
        DBGLOG(">%s> %s with select(%s)", m_name.str(), m_tagname.str(), m_select.get() ? m_select->getXpath() : "");
#endif
    }
};

class CEsdlTransformOperationHttpContentXml : public CEsdlTransformOperationWithChildren
{

public:
    CEsdlTransformOperationHttpContentXml(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix, IEsdlFunctionRegister *functionRegister)
        : CEsdlTransformOperationWithChildren(xpp, stag, prefix, true, functionRegister, nullptr)
    {
    }

    virtual ~CEsdlTransformOperationHttpContentXml(){}

    virtual bool exec(CriticalSection *crit, IInterface *preparedForAsync, IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext) override
    {
        OptionalCriticalBlock block(crit);

        CXpathContextLocation location(targetContext);
        targetContext->addElementToLocation("content");
        return processChildren(scriptContext, targetContext, sourceContext);
    }

    virtual void toDBGLog () override
    {
    #if defined(_DEBUG)
        DBGLOG (">>>>>>>>>>> %s >>>>>>>>>>", m_tagname.str());
        CEsdlTransformOperationWithChildren::toDBGLog();
        DBGLOG (">>>>>>>>>>> %s >>>>>>>>>>", m_tagname.str());
    #endif
    }
};

interface IEsdlTransformOperationHttpHeader : public IInterface
{
    virtual bool processHeader(IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext, IProperties *headers) = 0;
};

static Owned<ILoadedDllEntry> mysqlPluginDll;
static Owned<IEmbedContext> mysqlplugin;

IEmbedContext &ensureMysqlEmbed()
{
    if (!mysqlplugin)
    {
        mysqlPluginDll.setown(createDllEntry("mysqlembed", false, NULL, false));
        if (!mysqlPluginDll)
            throw makeStringException(0, "Failed to load mysqlembed plugin");
        GetEmbedContextFunction pf = (GetEmbedContextFunction) mysqlPluginDll->getEntry("getEmbedContextDynamic");
        if (!pf)
            throw makeStringException(0, "Failed to load mysqlembed plugin");
        mysqlplugin.setown(pf());
    }
    return *mysqlplugin;
}

class CEsdlTransformOperationMySqlBindParmeter : public CEsdlTransformOperationWithoutChildren
{
protected:
    StringAttr m_name;
    StringAttr m_mysql_type;
    Owned<ICompiledXpath> m_value;
    bool m_bitfield = false;

public:
    IMPLEMENT_IINTERFACE_USING(CEsdlTransformOperationWithoutChildren)

    CEsdlTransformOperationMySqlBindParmeter(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix) : CEsdlTransformOperationWithoutChildren(xpp, stag, prefix)
    {
        m_name.set(stag.getValue("name"));
        if (m_name.isEmpty())
            esdlOperationError(ESDL_SCRIPT_MissingOperationAttr, m_tagname, "without name or xpath_name", m_traceName, !m_ignoreCodingErrors);

        const char *value = stag.getValue("value");
        if (isEmptyString(value))
            esdlOperationError(ESDL_SCRIPT_MissingOperationAttr, m_tagname, "without value", m_traceName, !m_ignoreCodingErrors);
        m_value.setown(compileXpath(value));

        //optional, conversions normally work well, ONLY WHEN NEEDED we may need to have special handling for mysql types
        m_mysql_type.set(stag.getValue("type"));
        if (m_mysql_type.length() && 0==strnicmp(m_mysql_type.str(), "BIT", 3))
            m_bitfield = true;
    }

    virtual ~CEsdlTransformOperationMySqlBindParmeter(){}

    virtual bool exec(CriticalSection *crit, IInterface *preparedForAsync, IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext) override
    {
        return false;
    }

    void bindParameter(IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext, IEmbedFunctionContext *functionContext)
    {
        if (!functionContext)
            return;
        StringBuffer value;
        if (m_value)
            sourceContext->evaluateAsString(m_value, value);
        if (value.isEmpty())
            functionContext->bindUTF8Param(m_name, 0, "");
        else
        {
            if (m_bitfield)
                functionContext->bindSignedParam(m_name, atoi64(value.str()));
            else
                functionContext->bindUTF8Param(m_name, rtlUtf8Length(value.length(), value), value);
        }
    }

    virtual void toDBGLog () override
    {
    #if defined(_DEBUG)
        DBGLOG ("> %s (%s, value(%s)) >>>>>>>>>>", m_tagname.str(), m_name.str(), m_value ? m_value->getXpath() : "");
    #endif
    }
};

static inline void buildMissingMySqlParameterMessage(StringBuffer &msg, const char *name)
{
    msg .append(msg.isEmpty() ? "without " : ", ").append(name);
}

static inline void addExceptionsToXpathContext(IXpathContext *targetContext, IMultiException *me)
{
    if (!targetContext || !me)
        return;
    StringBuffer xml;
    me->serialize(xml);
    CXpathContextLocation content_location(targetContext);
    targetContext->ensureSetValue("@status", "error", true);
    targetContext->addXmlContent(xml.str());
}

static inline void addExceptionsToXpathContext(IXpathContext *targetContext, IException *E)
{
    if (!targetContext || !E)
        return;
    Owned<IMultiException> me = makeMultiException("ESDLScript");
    me->append(*LINK(E));
    addExceptionsToXpathContext(targetContext, me);
}

class CEsdlTransformOperationMySqlCall : public CEsdlTransformOperationBase
{
protected:
    StringAttr m_name;

    Owned<ICompiledXpath> m_vaultName;
    Owned<ICompiledXpath> m_secretName;
    Owned<ICompiledXpath> m_section;
    Owned<ICompiledXpath> m_resultsetTag;
    Owned<ICompiledXpath> m_server;
    Owned<ICompiledXpath> m_user;
    Owned<ICompiledXpath> m_password;
    Owned<ICompiledXpath> m_database;

    StringArray m_mysqlOptionNames;
    IArrayOf<ICompiledXpath> m_mysqlOptionXpaths;

    StringBuffer m_sql;

    Owned<ICompiledXpath> m_select;

    IArrayOf<CEsdlTransformOperationMySqlBindParmeter> m_parameters;

public:
    CEsdlTransformOperationMySqlCall(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix) : CEsdlTransformOperationBase(xpp, stag, prefix)
    {
        ensureMysqlEmbed();

        m_name.set(stag.getValue("name"));
        if (m_traceName.isEmpty())
            m_traceName.set(m_name.str());

        //select is optional, with select, a mysql call behaves like a for-each, binding and executing each iteration of the selected content
        //without select, it executes once in the current context
        m_select.setown(compileOptionalXpath(stag.getValue("select")));

        m_vaultName.setown(compileOptionalXpath(stag.getValue("vault")));
        m_secretName.setown(compileOptionalXpath(stag.getValue("secret")));
        m_section.setown(compileOptionalXpath(stag.getValue("section")));
        m_resultsetTag.setown(compileOptionalXpath(stag.getValue("resultset-tag")));

        m_server.setown(compileOptionalXpath(stag.getValue("server")));
        m_user.setown(compileOptionalXpath(stag.getValue("user")));
        m_password.setown(compileOptionalXpath(stag.getValue("password")));
        m_database.setown(compileOptionalXpath(stag.getValue("database")));

        //script can set any MYSQL options using an attribute with the same name as the option enum, for example
        //    MYSQL_SET_CHARSET_NAME="'latin1'" or MYSQL_SET_CHARSET_NAME="$charset"
        //
        int attCount = stag.getLength();
        for (int i=0; i<attCount; i++)
        {
            const char *attName = stag.getLocalName(i);
            if (attName && hasPrefix(attName, "MYSQL_", false))
            {
                Owned<ICompiledXpath> attXpath = compileOptionalXpath(stag.getValue(i));
                if (attXpath)
                {
                    m_mysqlOptionNames.append(attName);
                    m_mysqlOptionXpaths.append(*attXpath.getClear());
                }
            }
        }

        int type = 0;
        while((type = xpp.next()) != XmlPullParser::END_DOCUMENT)
        {
            switch(type)
            {
                case XmlPullParser::START_TAG:
                {
                    StartTag stag;
                    xpp.readStartTag(stag);
                    const char *op = stag.getLocalName();
                    if (isEmptyString(op))
                        esdlOperationError(ESDL_SCRIPT_Error, m_tagname, "unknown error", m_traceName, !m_ignoreCodingErrors);
                    if (streq(op, "bind"))
                        m_parameters.append(*new CEsdlTransformOperationMySqlBindParmeter(xpp, stag, prefix));
                    else if (streq(op, "sql"))
                        readFullContent(xpp, m_sql);
                    else
                        xpp.skipSubTreeEx();
                    break;
                }
                case XmlPullParser::END_TAG:
                case XmlPullParser::END_DOCUMENT:
                    return;
            }
        }

        if (!m_section)
            m_section.setown(compileXpath("'temporaries'"));
        StringBuffer errmsg;
        if (m_name.isEmpty())
            buildMissingMySqlParameterMessage(errmsg, "name");
        if (!m_server)
            buildMissingMySqlParameterMessage(errmsg, "server");
        if (!m_user)
            buildMissingMySqlParameterMessage(errmsg, "user");
        if (!m_database)
            buildMissingMySqlParameterMessage(errmsg, "database");
        if (m_sql.isEmpty())
            buildMissingMySqlParameterMessage(errmsg, "sql");
        if (errmsg.length())
            esdlOperationError(ESDL_SCRIPT_MissingOperationAttr, m_tagname, errmsg, m_traceName, !m_ignoreCodingErrors);
    }

    virtual ~CEsdlTransformOperationMySqlCall()
    {
    }

    void bindParameters(IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext, IEmbedFunctionContext *functionContext)
    {
        if (!m_parameters.length())
            return;
        CXpathContextLocation location(targetContext);
        ForEachItemIn(i, m_parameters)
            m_parameters.item(i).bindParameter(scriptContext, targetContext, sourceContext, functionContext);
    }

    void missingMySqlOptionError(const char *name, bool required)
    {
        if (required)
        {
            StringBuffer msg("empty or missing ");
            esdlOperationError(ESDL_SCRIPT_MissingOperationAttr, m_tagname, msg.append(name), m_traceName, true);
        }
    }
    IPropertyTree *getSecretInfo(IXpathContext * sourceContext)
    {
        //leaving flexibility for the secret to be configured multiple ways
        //  the most secure option in my opinion is to at least have the server, name, and password all in the secret
        //  with the server included the credentials can't be hijacked and sent somewhere else for capture.
        //
        if (!m_secretName)
            return nullptr;
        StringBuffer name;
        sourceContext->evaluateAsString(m_secretName, name);
        if (name.isEmpty())
        {
            missingMySqlOptionError(name, true);
            return nullptr;
        }
        StringBuffer vault;
        if (m_vaultName)
            sourceContext->evaluateAsString(m_vaultName, vault);
        if (vault.isEmpty())
            return getSecret("esp", name);
        return getVaultSecret("esp", vault, name);
    }
    void appendOption(StringBuffer &options, const char *name, const char *value, bool required)
    {
        if (isEmptyString(value))
        {
            missingMySqlOptionError(name, required);
            return;
        }
        if (options.length())
            options.append(',');
        options.append(name).append('=').append(value);

    }
    void appendOption(StringBuffer &options, const char *name, IXpathContext * sourceContext, ICompiledXpath *cx, IPropertyTree *secret, bool required)
    {
        if (secret && secret->hasProp(name))
        {
            StringBuffer value;
            getSecretKeyValue(value, secret, name);
            appendOption(options, name, value, required);
            return;
        }

        if (!cx)
        {
            missingMySqlOptionError(name, required);
            return;
        }
        StringBuffer value;
        sourceContext->evaluateAsString(cx, value);
        if (!value.length())
        {
            missingMySqlOptionError(name, required);
            return;
        }
        if (options.length())
            options.append(',');
        options.append(name).append('=').append(value);
    }
    IEmbedFunctionContext *createFunctionContext(IXpathContext * sourceContext)
    {
        Owned<IPropertyTree> secret = getSecretInfo(sourceContext);
        StringBuffer options;
        appendOption(options, "server", sourceContext, m_server, secret, true);
        appendOption(options, "user", sourceContext, m_user, secret, true);
        appendOption(options, "database", sourceContext, m_database, secret, true);
        appendOption(options, "password", sourceContext, m_password, secret, true);

        aindex_t count = m_mysqlOptionNames.length();
        for (aindex_t i=0; i<count; i++)
            appendOption(options, m_mysqlOptionNames.item(i), sourceContext, &m_mysqlOptionXpaths.item(i), nullptr, true);

        Owned<IEmbedFunctionContext> fc = ensureMysqlEmbed().createFunctionContext(EFembed, options.str());
        fc->compileEmbeddedScript(m_sql.length(), m_sql);
        return fc.getClear();
    }

    void processCurrent(IEmbedFunctionContext *fc, IXmlWriter *writer, const char *tag, IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext)
    {
        bindParameters(scriptContext, targetContext, sourceContext, fc);
        fc->callFunction();
        if (!isEmptyString(tag))
            writer->outputBeginNested(tag, true);
        fc->writeResult(nullptr, nullptr, nullptr, writer);
        if (!isEmptyString(tag))
            writer->outputEndNested(tag);
    }

    IXpathContextIterator *select(IXpathContext * xpathContext)
    {
        IXpathContextIterator *xpathset = nullptr;
        try
        {
            xpathset = xpathContext->evaluateAsNodeSet(m_select);
        }
        catch (IException* e)
        {
            int code = e->errorCode();
            StringBuffer msg;
            e->errorMessage(msg);
            e->Release();
            esdlOperationError(code, m_tagname, msg, !m_ignoreCodingErrors);
        }
        catch (...)
        {
            VStringBuffer msg("unknown exception evaluating select '%s'", m_select.get() ? m_select->getXpath() : "undefined!");
            esdlOperationError(ESDL_SCRIPT_Error, m_tagname, msg, !m_ignoreCodingErrors);
        }
        return xpathset;
    }

    void getXpathStringValue(StringBuffer &s, IXpathContext * sourceContext, ICompiledXpath *cx, const char *defaultValue)
    {
        if (cx)
            sourceContext->evaluateAsString(cx, s);
        if (defaultValue && s.isEmpty())
            s.set(defaultValue);
    }
    virtual bool exec(CriticalSection *crit, IInterface *preparedForAsync, IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext) override
    {
        //OperationMySqlCall is optimized to write directly into the document object
        //  In future we can create a different version that is optimized to work more asynchronously
        //  If we did, then inside a synchronous tag we might to have it optional which mode is used. For example efficiently streaming in data while
        //  an HTTP call is being made to an external service may still work best in the current mode
        OptionalCriticalBlock block(crit);

        StringBuffer section;
        getXpathStringValue(section, sourceContext, m_section, "temporaries");

        VStringBuffer xpath("/esdl_script_context/%s/%s", section.str(), m_name.str());
        CXpathContextLocation location(targetContext);
        targetContext->ensureLocation(xpath, true);

        Owned<IXpathContextIterator> selected;
        if (m_select)
        {
            selected.setown(select(sourceContext));
            if (!selected || !selected->first())
                return false;
        }

        try
        {
            Owned<IEmbedFunctionContext> fc = createFunctionContext(sourceContext);
            Owned<IXmlWriter> writer = targetContext->createXmlWriter();
            StringBuffer rstag;
            getXpathStringValue(rstag, sourceContext, m_resultsetTag, nullptr);
            if (!selected)
                processCurrent(fc, writer, rstag, scriptContext, targetContext, sourceContext);
            else
            {
                ForEach(*selected)
                    processCurrent(fc, writer, rstag, scriptContext, targetContext, &selected->query());
            }
        }
        catch(IMultiException *me)
        {
            addExceptionsToXpathContext(targetContext, me);
            me->Release();
        }
        catch(IException *E)
        {
            addExceptionsToXpathContext(targetContext, E);
            E->Release();
        }

        sourceContext->addXpathVariable(m_name, xpath);
        return true;
    }

    virtual void toDBGLog() override
    {
#if defined(_DEBUG)
        DBGLOG(">%s> %s with name(%s) server(%s) database(%s)", m_name.str(), m_tagname.str(), m_name.str(), m_server->getXpath(), m_database->getXpath());
#endif
    }
};

class CEsdlTransformOperationHttpHeader : public CEsdlTransformOperationWithoutChildren, implements IEsdlTransformOperationHttpHeader
{
protected:
    StringAttr m_name;
    Owned<ICompiledXpath> m_xpath_name;
    Owned<ICompiledXpath> m_value;

public:
    IMPLEMENT_IINTERFACE_USING(CEsdlTransformOperationWithoutChildren)

    CEsdlTransformOperationHttpHeader(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix) : CEsdlTransformOperationWithoutChildren(xpp, stag, prefix)
    {
        m_name.set(stag.getValue("name"));
        const char *xpath_name = stag.getValue("xpath_name");
        if (m_name.isEmpty() && isEmptyString(xpath_name))
            esdlOperationError(ESDL_SCRIPT_MissingOperationAttr, m_tagname, "without name or xpath_name", m_traceName, !m_ignoreCodingErrors);
        if (!isEmptyString(xpath_name))
            m_xpath_name.setown(compileXpath(xpath_name));

        const char *value = stag.getValue("value");
        if (isEmptyString(value))
            esdlOperationError(ESDL_SCRIPT_MissingOperationAttr, m_tagname, "without value", m_traceName, !m_ignoreCodingErrors);
        m_value.setown(compileXpath(value));
    }

    virtual ~CEsdlTransformOperationHttpHeader(){}

    virtual bool exec(CriticalSection *crit, IInterface *preparedForAsync, IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext) override
    {
        OptionalCriticalBlock block(crit);
        return processHeader(scriptContext, targetContext, sourceContext, nullptr);
    }

    bool processHeader(IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext, IProperties *headers) override
    {
        CXpathContextLocation location(targetContext);
        targetContext->addElementToLocation("header");
        StringBuffer name;
        if (m_xpath_name)
            sourceContext->evaluateAsString(m_xpath_name, name);
        else
            name.set(m_name);

        StringBuffer value;
        if (m_value)
            sourceContext->evaluateAsString(m_value, value);
        if (name.length() && value.length())
        {
            if (headers)
                headers->setProp(name, value);
            targetContext->ensureSetValue("@name", name, true);
            targetContext->ensureSetValue("@value", value, true);
        }
        return false;
    }

    virtual void toDBGLog () override
    {
    #if defined(_DEBUG)
        DBGLOG ("> %s (%s, value(%s)) >>>>>>>>>>", m_tagname.str(), m_xpath_name ? m_xpath_name->getXpath() : m_name.str(), m_value ? m_value->getXpath() : "");
    #endif
    }
};

class OperationStateHttpPostXml : public CInterfaceOf<IInterface> //plain CInterface doesn't actually give us our opaque IInterface pointer
{
public:
    Owned<IProperties> headers = createProperties();
    StringBuffer url;
    StringBuffer content;
    unsigned testDelay = 0;
};


class CEsdlTransformOperationHttpPostXml : public CEsdlTransformOperationBase
{
protected:
    StringAttr m_name;
    StringAttr m_section;
    Owned<ICompiledXpath> m_url;
    IArrayOf<IEsdlTransformOperationHttpHeader> m_headers;
    Owned<IEsdlTransformOperation> m_content;
    Owned<ICompiledXpath> m_testDelay;

public:
    CEsdlTransformOperationHttpPostXml(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix, IEsdlFunctionRegister *functionRegister) : CEsdlTransformOperationBase(xpp, stag, prefix)
    {
        m_name.set(stag.getValue("name"));
        if (m_traceName.isEmpty())
            m_traceName.set(m_name.str());
        m_section.set(stag.getValue("section"));
        if (m_section.isEmpty())
            m_section.set("temporaries");
        if (m_name.isEmpty())
            esdlOperationError(ESDL_SCRIPT_MissingOperationAttr, m_tagname, "without name", m_traceName, !m_ignoreCodingErrors);
        const char *url = stag.getValue("url");
        if (isEmptyString(url))
            esdlOperationError(ESDL_SCRIPT_MissingOperationAttr, m_tagname, "without url", m_traceName, !m_ignoreCodingErrors);
        m_url.setown(compileXpath(url));
        const char *msTestDelayStr = stag.getValue("test-delay");
        if (!isEmptyString(msTestDelayStr))
            m_testDelay.setown(compileXpath(msTestDelayStr));

        int type = 0;
        while((type = xpp.next()) != XmlPullParser::END_DOCUMENT)
        {
            switch(type)
            {
                case XmlPullParser::START_TAG:
                {
                    StartTag stag;
                    xpp.readStartTag(stag);
                    const char *op = stag.getLocalName();
                    if (isEmptyString(op))
                        esdlOperationError(ESDL_SCRIPT_Error, m_tagname, "unknown error", m_traceName, !m_ignoreCodingErrors);
                    if (streq(op, "http-header"))
                        m_headers.append(*new CEsdlTransformOperationHttpHeader(xpp, stag, prefix));
                    else if (streq(op, "content"))
                        m_content.setown(new CEsdlTransformOperationHttpContentXml(xpp, stag, prefix, functionRegister));
                    else
                        xpp.skipSubTreeEx();
                    break;
                }
                case XmlPullParser::END_TAG:
                case XmlPullParser::END_DOCUMENT:
                    return;
            }
        }
    }

    virtual ~CEsdlTransformOperationHttpPostXml()
    {
    }

    void buildHeaders(IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext, IProperties *headers)
    {
        if (!m_headers.length())
            return;
        CXpathContextLocation location(targetContext);
        targetContext->addElementToLocation("headers");
        ForEachItemIn(i, m_headers)
            m_headers.item(i).processHeader(scriptContext, targetContext, sourceContext, headers);
        if (!headers->hasProp("Accept"))
            headers->setProp("Accept", "text/html, application/xml");
    }

    void buildRequest(OperationStateHttpPostXml &preparedState, IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext)
    {
        CXpathContextLocation location(targetContext);
        targetContext->addElementToLocation("request");
        targetContext->ensureSetValue("@url", preparedState.url, true);
        buildHeaders(scriptContext, targetContext, sourceContext, preparedState.headers);
        if (m_content)
            m_content->process(scriptContext, targetContext, sourceContext);
        VStringBuffer xpath("/esdl_script_context/%s/%s/request/content/*[1]", m_section.str(), m_name.str());
        sourceContext->toXml(xpath, preparedState.content);
    }

    virtual IInterface *prepareForAsync(IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext) override
    {
        Owned<OperationStateHttpPostXml> preparedState = new OperationStateHttpPostXml;
        CXpathContextLocation location(targetContext);
        VStringBuffer xpath("/esdl_script_context/%s/%s", m_section.str(), m_name.str());
        targetContext->ensureLocation(xpath, true);
        if (m_url)
            sourceContext->evaluateAsString(m_url, preparedState->url);

        //don't complain if test-delay is used but test mode is off. Any script can be instrumented for testing but won't run those features outside of testing
        if (scriptContext->getTestMode() && m_testDelay)
            preparedState->testDelay = (unsigned) sourceContext->evaluateAsNumber(m_testDelay);

        buildRequest(*preparedState, scriptContext, targetContext, sourceContext);
        return preparedState.getClear();
    }

    virtual bool process(IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext) override
    {
        //if process is called here we are not currently a child of "synchronize", but because we do support synchronize
        //  we keep our state isolated.  Therefor unlike other operations we still need to prepare our "pre async" state object before calling exec
        //  in future additional operations may be optimized for synchronize in this way.
        Owned<IInterface> state = prepareForAsync(scriptContext, targetContext, sourceContext);
        return exec(nullptr, state, scriptContext, targetContext, sourceContext);
    }

    virtual bool exec(CriticalSection *crit, IInterface *preparedForAsync, IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext) override
    {
        OperationStateHttpPostXml &preparedState = *static_cast<OperationStateHttpPostXml *>(preparedForAsync);
        if (preparedState.content.isEmpty())
            return false;
        HttpClientErrCode code = HttpClientErrCode::OK;
        Owned<IHttpMessage> resp;

        StringBuffer err;
        StringBuffer status;
        StringBuffer contentType;
        StringBuffer response;
        StringBuffer exceptionXml;

        try
        {
            Owned<IHttpClientContext> httpCtx = getHttpClientContext();
            Owned<IHttpClient> httpclient = httpCtx->createHttpClient(NULL, preparedState.url);
            if (!httpclient)
                return false;

            StringBuffer errmsg;
            resp.setown(httpclient->sendRequestEx("POST", "text/xml", preparedState.content, code, errmsg, preparedState.headers));
            err.append((int) code);

            if (code != HttpClientErrCode::OK)
                throw MakeStringException(ESDL_SCRIPT_Error, "ESDL Script error sending request in http-post-xml %s url(%s)", m_traceName.str(), preparedState.url.str());

            resp->getContent(response);
            if (!response.trim().length())
                throw MakeStringException(ESDL_SCRIPT_Error, "ESDL Script empty result calling http-post-xml %s url(%s)", m_traceName.str(), preparedState.url.str());

            resp->getStatus(status);
            resp->getContentType(contentType);
        }
        catch(IMultiException *me)
        {
            me->serialize(exceptionXml);
            me->Release();
        }
        catch(IException *E)
        {
            Owned<IMultiException> me = makeMultiException("ESDLScript");
            me->append(*LINK(E));
            me->serialize(exceptionXml);
            E->Release();
        }

        if (preparedState.testDelay)
            MilliSleep(preparedState.testDelay);

        VStringBuffer xpath("/esdl_script_context/%s/%s", m_section.str(), m_name.str());

        //No need to synchronize until we hit this point and start updating the scriptContext / xml document
        OptionalCriticalBlock block(crit);

        CXpathContextLocation location(targetContext);
        targetContext->ensureLocation(xpath, true);

        if (exceptionXml.length())
        {
            CXpathContextLocation content_location(targetContext);
            targetContext->addElementToLocation("content");
            targetContext->ensureSetValue("@status", "error", true);
            targetContext->addXmlContent(exceptionXml.str());
        }
        else
        {
            CXpathContextLocation response_location(targetContext);
            targetContext->addElementToLocation("response");
            targetContext->ensureSetValue("@status", status.str(), true);
            targetContext->ensureSetValue("@error-code", err.str(), true);
            targetContext->ensureSetValue("@content-type", contentType.str(), true);
            if (strnicmp("text/xml", contentType.str(), 8)==0 || strnicmp("application/xml", contentType.str(), 15) ==0)
            {
                CXpathContextLocation content_location(targetContext);
                targetContext->addElementToLocation("content");
                targetContext->addXmlContent(response.str());
            }
            else
            {
                targetContext->ensureSetValue("text", response.str(), true);
            }
        }

        sourceContext->addXpathVariable(m_name, xpath);
        return false;
    }

    virtual void toDBGLog() override
    {
#if defined(_DEBUG)
        DBGLOG(">%s> %s with name(%s) url(%s)", m_name.str(), m_tagname.str(), m_name.str(), m_url ? m_url->getXpath() : "url error");
#endif
    }
};

class CEsdlTransformOperationParameter : public CEsdlTransformOperationVariable
{
public:
    CEsdlTransformOperationParameter(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix, IEsdlFunctionRegister *functionRegister)
        : CEsdlTransformOperationVariable(xpp, stag, prefix, functionRegister)
    {
    }

    virtual ~CEsdlTransformOperationParameter()
    {
    }

    virtual bool exec(CriticalSection *crit, IInterface *preparedForAsync, IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext) override
    {
        OptionalCriticalBlock block(crit);

        if (m_select)
            return sourceContext->declareCompiledParameter(m_name, m_select);
        return sourceContext->declareParameter(m_name, "");
    }
};

class CEsdlTransformOperationSetSectionAttributeBase : public CEsdlTransformOperationWithoutChildren
{
protected:
    StringAttr m_name;
    Owned<ICompiledXpath> m_xpath_name;
    Owned<ICompiledXpath> m_select;

public:
    CEsdlTransformOperationSetSectionAttributeBase(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix, const char *attrName) : CEsdlTransformOperationWithoutChildren(xpp, stag, prefix)
    {
        if (m_traceName.isEmpty())
            m_traceName.set(stag.getValue("name"));
        if (!isEmptyString(attrName))
            m_name.set(attrName);
        else
        {
            m_name.set(stag.getValue("name"));

            const char *xpath_name = stag.getValue("xpath_name");
            if (!isEmptyString(xpath_name))
                m_xpath_name.setown(compileXpath(xpath_name));
            else if (m_name.isEmpty())
                esdlOperationError(ESDL_SCRIPT_MissingOperationAttr, m_tagname, "without name", m_traceName, !m_ignoreCodingErrors); //don't mention value, it's deprecated
        }

        const char *select = stag.getValue("select");
        if (isEmptyString(select))
            esdlOperationError(ESDL_SCRIPT_MissingOperationAttr, m_tagname, "without select", m_traceName, !m_ignoreCodingErrors); //don't mention value, it's deprecated
        m_select.setown(compileXpath(select));
    }

    virtual void toDBGLog() override
    {
#if defined(_DEBUG)
        DBGLOG(">%s> %s(name(%s), select('%s'))", m_traceName.str(), m_tagname.str(), (m_xpath_name) ? m_xpath_name->getXpath() : m_name.str(), m_select->getXpath());
#endif
    }

    virtual ~CEsdlTransformOperationSetSectionAttributeBase(){}

    virtual const char *getSectionName() = 0;

    virtual bool exec(CriticalSection *crit, IInterface *preparedForAsync, IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext) override
    {
        OptionalCriticalBlock block(crit);

        if ((!m_name && !m_xpath_name) || !m_select)
            return false; //only here if "optional" backward compatible support for now (optional syntax errors aren't actually helpful)
        try
        {
            StringBuffer name;
            if (m_xpath_name)
                sourceContext->evaluateAsString(m_xpath_name, name);
            else
                name.set(m_name);

            StringBuffer value;
            sourceContext->evaluateAsString(m_select, value);
            scriptContext->setAttribute(getSectionName(), name, value);
        }
        catch (IException* e)
        {
            int code = e->errorCode();
            StringBuffer msg;
            e->errorMessage(msg);
            e->Release();
            esdlOperationError(code, m_tagname, msg, m_traceName, !m_ignoreCodingErrors);
        }
        catch (...)
        {
            esdlOperationError(ESDL_SCRIPT_Error, m_tagname, "unknown exception processing", m_traceName, !m_ignoreCodingErrors);
        }
        return false;
    }
};

class CEsdlTransformOperationStoreValue : public CEsdlTransformOperationSetSectionAttributeBase
{
public:
    CEsdlTransformOperationStoreValue(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix) : CEsdlTransformOperationSetSectionAttributeBase(xpp, stag, prefix, nullptr)
    {
    }

    virtual ~CEsdlTransformOperationStoreValue(){}
    const char *getSectionName() override {return ESDLScriptCtxSection_Store;}
};

class CEsdlTransformOperationSetLogProfile : public CEsdlTransformOperationSetSectionAttributeBase
{
public:
    CEsdlTransformOperationSetLogProfile(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix) : CEsdlTransformOperationSetSectionAttributeBase(xpp, stag, prefix, "profile")
    {
    }

    virtual ~CEsdlTransformOperationSetLogProfile(){}
    const char *getSectionName() override {return ESDLScriptCtxSection_Logging;}
};

class CEsdlTransformOperationSetLogOption : public CEsdlTransformOperationSetSectionAttributeBase
{
public:
    CEsdlTransformOperationSetLogOption(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix) : CEsdlTransformOperationSetSectionAttributeBase(xpp, stag, prefix, nullptr)
    {
    }

    virtual ~CEsdlTransformOperationSetLogOption(){}
    const char *getSectionName() override {return ESDLScriptCtxSection_Logging;}
};

class CEsdlTransformOperationSetValue : public CEsdlTransformOperationWithoutChildren
{
protected:
    Owned<ICompiledXpath> m_select;
    Owned<ICompiledXpath> m_xpath_target;
    StringAttr m_target;
    bool m_required = true;

public:
    CEsdlTransformOperationSetValue(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix) : CEsdlTransformOperationWithoutChildren(xpp, stag, prefix)
    {
        if (m_traceName.isEmpty())
            m_traceName.set(stag.getValue("name"));

        const char *xpath_target = stag.getValue("xpath_target");
        const char *target = stag.getValue("target");

        if (isEmptyString(target) && isEmptyString(xpath_target))
            esdlOperationError(ESDL_SCRIPT_MissingOperationAttr, m_tagname.str(), "without target", m_traceName.str(), !m_ignoreCodingErrors);

        const char *select = stag.getValue("select");
        if (isEmptyString(select))
            select = stag.getValue("value");
        if (isEmptyString(select))
            esdlOperationError(ESDL_SCRIPT_MissingOperationAttr, m_tagname, "without select", m_traceName, !m_ignoreCodingErrors); //don't mention value, it's deprecated

        m_select.setown(compileXpath(select));
        if (!isEmptyString(xpath_target))
            m_xpath_target.setown(compileXpath(xpath_target));
        else if (!isEmptyString(target))
            m_target.set(target);
        m_required = getStartTagValueBool(stag, "required", true);
    }

    virtual void toDBGLog() override
    {
#if defined(_DEBUG)
        DBGLOG(">%s> %s(%s, select('%s'))", m_traceName.str(), m_tagname.str(), m_target.str(), m_select->getXpath());
#endif
    }

    virtual ~CEsdlTransformOperationSetValue(){}

    virtual bool exec(CriticalSection *crit, IInterface *preparedForAsync, IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext) override
    {
        OptionalCriticalBlock block(crit);

        if ((!m_xpath_target && m_target.isEmpty()) || !m_select)
            return false; //only here if "optional" backward compatible support for now (optional syntax errors aren't actually helpful
        try
        {
            StringBuffer value;
            sourceContext->evaluateAsString(m_select, value);
            return doSet(sourceContext, targetContext, value);
        }
        catch (IException* e)
        {
            int code = e->errorCode();
            StringBuffer msg;
            e->errorMessage(msg);
            e->Release();
            esdlOperationError(code, m_tagname, msg, m_traceName, !m_ignoreCodingErrors);
        }
        catch (...)
        {
            esdlOperationError(ESDL_SCRIPT_Error, m_tagname, "unknown exception processing", m_traceName, !m_ignoreCodingErrors);
        }
        return false;
    }

    const char *getTargetPath(IXpathContext * xpathContext, StringBuffer &s)
    {
        if (m_xpath_target)
        {
            xpathContext->evaluateAsString(m_xpath_target, s);
            return s;
        }
        return m_target.str();
    }
    virtual bool doSet(IXpathContext * sourceContext, IXpathContext *targetContext, const char *value)
    {
        StringBuffer xpath;
        const char *target = getTargetPath(sourceContext, xpath);
        targetContext->ensureSetValue(target, value, m_required);
        return true;
    }
};

class CEsdlTransformOperationNamespace : public CEsdlTransformOperationWithoutChildren
{
protected:
    StringAttr m_prefix;
    StringAttr m_uri;
    bool m_current = false;

public:
    CEsdlTransformOperationNamespace(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix) : CEsdlTransformOperationWithoutChildren(xpp, stag, prefix)
    {
        const char *pfx = stag.getValue("prefix");
        const char *uri = stag.getValue("uri");
        if (m_traceName.isEmpty())
            m_traceName.set(pfx);

        if (!pfx && isEmptyString(uri))
            esdlOperationError(ESDL_SCRIPT_MissingOperationAttr, m_tagname.str(), "without prefix or uri", m_traceName.str(), !m_ignoreCodingErrors);
        m_uri.set(uri);
        m_prefix.set(pfx);
        m_current = getStartTagValueBool(stag, "current", m_uri.isEmpty());
    }

    virtual void toDBGLog() override
    {
#if defined(_DEBUG)
        DBGLOG(">%s> %s(prefix('%s'), uri('%s'), current(%d))", m_traceName.str(), m_tagname.str(), m_prefix.str(), m_uri.str(), m_current);
#endif
    }

    virtual ~CEsdlTransformOperationNamespace(){}

    virtual bool exec(CriticalSection *crit, IInterface *preparedForAsync, IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext) override
    {
        OptionalCriticalBlock block(crit);

        targetContext->setLocationNamespace(m_prefix, m_uri, m_current);
        return false;
    }
};

class CEsdlTransformOperationRenameNode : public CEsdlTransformOperationWithoutChildren
{
protected:
    StringAttr m_target;
    StringAttr m_new_name;
    Owned<ICompiledXpath> m_xpath_target;
    Owned<ICompiledXpath> m_xpath_new_name;
    bool m_all = false;

public:
    CEsdlTransformOperationRenameNode(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix) : CEsdlTransformOperationWithoutChildren(xpp, stag, prefix)
    {
        const char *new_name = stag.getValue("new_name");
        const char *xpath_new_name = stag.getValue("xpath_new_name");
        if (isEmptyString(new_name) && isEmptyString(xpath_new_name))
            esdlOperationError(ESDL_SCRIPT_MissingOperationAttr, m_tagname.str(), "without new name", m_traceName.str(), !m_ignoreCodingErrors);
        if (m_traceName.isEmpty())
            m_traceName.set(new_name ? new_name : xpath_new_name);

        const char *target = stag.getValue("target");
        const char *xpath_target = stag.getValue("xpath_target");
        if (isEmptyString(target) && isEmptyString(xpath_target))
            esdlOperationError(ESDL_SCRIPT_MissingOperationAttr, m_tagname.str(), "without target", m_traceName.str(), !m_ignoreCodingErrors);

        if (!isEmptyString(xpath_target))
            m_xpath_target.setown(compileXpath(xpath_target));
        else if (!isEmptyString(target))
            m_target.set(target);

        if (!isEmptyString(xpath_new_name))
            m_xpath_new_name.setown(compileXpath(xpath_new_name));
        else if (!isEmptyString(new_name))
            m_new_name.set(new_name);
        m_all = getStartTagValueBool(stag, "all", false);
    }

    virtual void toDBGLog() override
    {
#if defined(_DEBUG)
        const char *target = (m_xpath_target) ? m_xpath_target->getXpath() : m_target.str();
        const char *new_name = (m_xpath_new_name) ? m_xpath_new_name->getXpath() : m_new_name.str();
        DBGLOG(">%s> %s(%s, new_name('%s'))", m_traceName.str(), m_tagname.str(), target, new_name);
#endif
    }

    virtual ~CEsdlTransformOperationRenameNode(){}

    virtual bool exec(CriticalSection *crit, IInterface *preparedForAsync, IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext) override
    {
        OptionalCriticalBlock block(crit);

        if ((!m_xpath_target && m_target.isEmpty()) || (!m_xpath_new_name && m_new_name.isEmpty()))
            return false; //only here if "optional" backward compatible support for now (optional syntax errors aren't actually helpful
        try
        {
            StringBuffer path;
            if (m_xpath_target)
                sourceContext->evaluateAsString(m_xpath_target, path);
            else
                path.set(m_target);

            StringBuffer name;
            if (m_xpath_new_name)
                sourceContext->evaluateAsString(m_xpath_new_name, name);
            else
                name.set(m_new_name);

            targetContext->rename(path, name, m_all);
        }
        catch (IException* e)
        {
            int code = e->errorCode();
            StringBuffer msg;
            e->errorMessage(msg);
            e->Release();
            esdlOperationError(code, m_tagname, msg, m_traceName, !m_ignoreCodingErrors);
        }
        catch (...)
        {
            esdlOperationError(ESDL_SCRIPT_Error, m_tagname, "unknown exception processing", m_traceName, !m_ignoreCodingErrors);
        }
        return false;
    }
};

class CEsdlTransformOperationCopyOf : public CEsdlTransformOperationWithoutChildren
{
protected:
    Owned<ICompiledXpath> m_select;
    StringAttr m_new_name;

public:
    CEsdlTransformOperationCopyOf(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix) : CEsdlTransformOperationWithoutChildren(xpp, stag, prefix)
    {
        const char *select = stag.getValue("select");
        if (isEmptyString(select))
            esdlOperationError(ESDL_SCRIPT_MissingOperationAttr, m_tagname.str(), "without select", m_traceName.str(), !m_ignoreCodingErrors);

        m_select.setown(compileXpath(select));
        m_new_name.set(stag.getValue("new_name"));
    }

    virtual void toDBGLog() override
    {
#if defined(_DEBUG)
        DBGLOG(">%s> %s(%s, new_name('%s'))", m_traceName.str(), m_tagname.str(), m_select->getXpath(), m_new_name.str());
#endif
    }

    virtual ~CEsdlTransformOperationCopyOf(){}

    virtual bool exec(CriticalSection *crit, IInterface *preparedForAsync, IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext) override
    {
        OptionalCriticalBlock block(crit);

        try
        {
            targetContext->copyFromPrimaryContext(m_select, m_new_name);
        }
        catch (IException* e)
        {
            int code = e->errorCode();
            StringBuffer msg;
            e->errorMessage(msg);
            e->Release();
            esdlOperationError(code, m_tagname, msg, m_traceName, !m_ignoreCodingErrors);
        }
        catch (...)
        {
            esdlOperationError(ESDL_SCRIPT_Error, m_tagname, "unknown exception processing", m_traceName, !m_ignoreCodingErrors);
        }
        return false;
    }
};

class CEsdlTransformOperationTrace : public CEsdlTransformOperationWithoutChildren
{
protected:
    StringAttr m_label;
    Owned<ICompiledXpath> m_test; //optional, if provided trace only if test evaluates to true
    Owned<ICompiledXpath> m_select;

public:
    CEsdlTransformOperationTrace(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix) : CEsdlTransformOperationWithoutChildren(xpp, stag, prefix)
    {
        m_label.set(stag.getValue("label"));
        const char *select = stag.getValue("select");
        if (isEmptyString(select))
            esdlOperationError(ESDL_SCRIPT_MissingOperationAttr, m_tagname.str(), "without select", m_traceName.str(), !m_ignoreCodingErrors);

        m_select.setown(compileXpath(select));

        const char *test = stag.getValue("test");
        if (!isEmptyString(test))
            m_test.setown(compileXpath(test));
    }

    virtual void toDBGLog() override
    {
#if defined(_DEBUG)
        DBGLOG(">%s> %s(test(%s), label(%s), select(%s))", m_traceName.str(), m_tagname.str(), m_test ? m_test->getXpath() : "true", m_label.str(), m_select->getXpath());
#endif
    }

    virtual ~CEsdlTransformOperationTrace(){}

    virtual bool exec(CriticalSection *crit, IInterface *preparedForAsync, IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext) override
    {
        OptionalCriticalBlock block(crit);

        try
        {
            if (m_test && !sourceContext->evaluateAsBoolean(m_test))
                return false;
            targetContext->trace(m_label, m_select, scriptContext->getTraceToStdout());
        }
        catch (IException* e)
        {
            int code = e->errorCode();
            StringBuffer msg;
            e->errorMessage(msg);
            e->Release();
            esdlOperationError(code, m_tagname, msg, m_traceName, !m_ignoreCodingErrors);
        }
        catch (...)
        {
            esdlOperationError(ESDL_SCRIPT_Error, m_tagname, "unknown exception processing", m_traceName, !m_ignoreCodingErrors);
        }
        return false;
    }
};

class CEsdlTransformOperationRemoveNode : public CEsdlTransformOperationWithoutChildren
{
protected:
    StringAttr m_target;
    Owned<ICompiledXpath> m_xpath_target;
    bool m_all = false;

public:
    CEsdlTransformOperationRemoveNode(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix) : CEsdlTransformOperationWithoutChildren(xpp, stag, prefix)
    {
        const char *target = stag.getValue("target");
        const char *xpath_target = stag.getValue("xpath_target");
        if (isEmptyString(target) && isEmptyString(xpath_target))
            esdlOperationError(ESDL_SCRIPT_MissingOperationAttr, m_tagname.str(), "without target", m_traceName.str(), !m_ignoreCodingErrors);
        if (target && isWildString(target))
            esdlOperationError(ESDL_SCRIPT_MissingOperationAttr, m_tagname.str(), "wildcard in target not yet supported", m_traceName.str(), !m_ignoreCodingErrors);

        if (!isEmptyString(xpath_target))
            m_xpath_target.setown(compileXpath(xpath_target));
        else if (!isEmptyString(target))
            m_target.set(target);
        m_all = getStartTagValueBool(stag, "all", false);
    }

    virtual void toDBGLog() override
    {
#if defined(_DEBUG)
        const char *target = (m_xpath_target) ? m_xpath_target->getXpath() : m_target.str();
        DBGLOG(">%s> %s(%s)", m_traceName.str(), m_tagname.str(), target);
#endif
    }

    virtual ~CEsdlTransformOperationRemoveNode(){}

    virtual bool exec(CriticalSection *crit, IInterface *preparedForAsync, IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext) override
    {
        OptionalCriticalBlock block(crit);

        if ((!m_xpath_target && m_target.isEmpty()))
            return false; //only here if "optional" backward compatible support for now (optional syntax errors aren't actually helpful
        try
        {
            StringBuffer path;
            if (m_xpath_target)
                sourceContext->evaluateAsString(m_xpath_target, path);
            else
                path.set(m_target);

            targetContext->remove(path, m_all);
        }
        catch (IException* e)
        {
            int code = e->errorCode();
            StringBuffer msg;
            e->errorMessage(msg);
            e->Release();
            esdlOperationError(code, m_tagname, msg, m_traceName, !m_ignoreCodingErrors);
        }
        catch (...)
        {
            esdlOperationError(ESDL_SCRIPT_Error, m_tagname, "unknown exception processing", m_traceName, !m_ignoreCodingErrors);
        }
        return false;
    }
};

class CEsdlTransformOperationAppendValue : public CEsdlTransformOperationSetValue
{
public:
    CEsdlTransformOperationAppendValue(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix) : CEsdlTransformOperationSetValue(xpp, stag, prefix){}

    virtual ~CEsdlTransformOperationAppendValue(){}

    virtual bool doSet(IXpathContext * sourceContext, IXpathContext *targetContext, const char *value) override
    {
        StringBuffer xpath;
        const char *target = getTargetPath(sourceContext, xpath);
        targetContext->ensureAppendToValue(target, value, m_required);
        return true;
    }
};

class CEsdlTransformOperationAddValue : public CEsdlTransformOperationSetValue
{
public:
    CEsdlTransformOperationAddValue(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix) : CEsdlTransformOperationSetValue(xpp, stag, prefix){}

    virtual ~CEsdlTransformOperationAddValue(){}

    virtual bool doSet(IXpathContext * sourceContext, IXpathContext *targetContext, const char *value) override
    {
        StringBuffer xpath;
        const char *target = getTargetPath(sourceContext, xpath);
        targetContext->ensureAddValue(target, value, m_required);
        return true;
    }
};

class CEsdlTransformOperationFail : public CEsdlTransformOperationWithoutChildren
{
protected:
    Owned<ICompiledXpath> m_message;
    Owned<ICompiledXpath> m_code;

public:
    CEsdlTransformOperationFail(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix) : CEsdlTransformOperationWithoutChildren(xpp, stag, prefix)
    {
        if (m_traceName.isEmpty())
            m_traceName.set(stag.getValue("name"));

        const char *code = stag.getValue("code");
        const char *message = stag.getValue("message");
        if (isEmptyString(code))
            esdlOperationError(ESDL_SCRIPT_MissingOperationAttr, m_tagname, "without code", m_traceName.str(), true);
        if (isEmptyString(message))
            esdlOperationError(ESDL_SCRIPT_MissingOperationAttr, m_tagname, "without message", m_traceName.str(), true);

        m_code.setown(compileXpath(code));
        m_message.setown(compileXpath(message));
    }

    virtual ~CEsdlTransformOperationFail()
    {
    }

    bool doFail(IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext)
    {
        int code = m_code.get() ? (int) sourceContext->evaluateAsNumber(m_code) : ESDL_SCRIPT_Error;
        StringBuffer msg;
        if (m_message.get())
            sourceContext->evaluateAsString(m_message, msg);
        throw makeStringException(code, msg.str());
        return true; //avoid compilation error
    }

    virtual bool exec(CriticalSection *crit, IInterface *preparedForAsync, IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext) override
    {
        OptionalCriticalBlock block(crit);
        return doFail(scriptContext, targetContext, sourceContext);
    }

    virtual void toDBGLog() override
    {
#if defined(_DEBUG)
        DBGLOG(">%s> %s with message(%s)", m_traceName.str(), m_tagname.str(), m_message.get() ? m_message->getXpath() : "");
#endif
    }
};

class CEsdlTransformOperationAssert : public CEsdlTransformOperationFail
{
private:
    Owned<ICompiledXpath> m_test; //assert is like a conditional fail

public:
    CEsdlTransformOperationAssert(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix) : CEsdlTransformOperationFail(xpp, stag, prefix)
    {
        const char *test = stag.getValue("test");
        if (isEmptyString(test))
            esdlOperationError(ESDL_SCRIPT_MissingOperationAttr, m_tagname, "without test", m_traceName.str(), true);
        m_test.setown(compileXpath(test));
    }

    virtual ~CEsdlTransformOperationAssert()
    {
    }

    virtual bool exec(CriticalSection *crit, IInterface *preparedForAsync, IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext) override
    {
        OptionalCriticalBlock block(crit);

        if (m_test && sourceContext->evaluateAsBoolean(m_test))
            return false;
        return CEsdlTransformOperationFail::doFail(scriptContext, targetContext, sourceContext);
    }

    virtual void toDBGLog() override
    {
#if defined(_DEBUG)
        const char *testXpath = m_test.get() ? m_test->getXpath() : "SYNTAX ERROR IN test";
        DBGLOG(">%s> %s if '%s' with message(%s)", m_traceName.str(), m_tagname.str(), testXpath, m_message.get() ? m_message->getXpath() : "");
#endif
    }
};

class CEsdlTransformOperationForEach : public CEsdlTransformOperationWithChildren
{
protected:
    Owned<ICompiledXpath> m_select;

public:
    CEsdlTransformOperationForEach(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix, IEsdlFunctionRegister *functionRegister)
        : CEsdlTransformOperationWithChildren(xpp, stag, prefix, true, functionRegister, nullptr)
    {
        const char *select = stag.getValue("select");
        if (isEmptyString(select))
            esdlOperationError(ESDL_SCRIPT_MissingOperationAttr, m_tagname, "without select", !m_ignoreCodingErrors);
        m_select.setown(compileXpath(select));
    }

    virtual ~CEsdlTransformOperationForEach(){}

    virtual bool exec(CriticalSection *crit, IInterface *preparedForAsync, IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext) override
    {
        OptionalCriticalBlock block(crit);

        Owned<IXpathContextIterator> contexts = evaluate(sourceContext);
        if (!contexts)
            return false;
        if (!contexts->first())
            return false;
        ForEach(*contexts)
            processChildren(scriptContext, targetContext, &contexts->query());
        return true;
    }

    virtual void toDBGLog () override
    {
    #if defined(_DEBUG)
        DBGLOG (">>>>%s %s ", m_tagname.str(), m_select ? m_select->getXpath() : "");
        CEsdlTransformOperationWithChildren::toDBGLog();
        DBGLOG ("<<<<%s<<<<<", m_tagname.str());
    #endif
    }

private:
    IXpathContextIterator *evaluate(IXpathContext * xpathContext)
    {
        IXpathContextIterator *xpathset = nullptr;
        try
        {
            xpathset = xpathContext->evaluateAsNodeSet(m_select);
        }
        catch (IException* e)
        {
            int code = e->errorCode();
            StringBuffer msg;
            e->errorMessage(msg);
            e->Release();
            esdlOperationError(code, m_tagname, msg, !m_ignoreCodingErrors);
        }
        catch (...)
        {
            VStringBuffer msg("unknown exception evaluating select '%s'", m_select.get() ? m_select->getXpath() : "undefined!");
            esdlOperationError(ESDL_SCRIPT_Error, m_tagname, msg, !m_ignoreCodingErrors);
        }
        return xpathset;
    }
};

class CEsdlTransformOperationSynchronize : public CEsdlTransformOperationWithChildren
{
    Owned<ICompiledXpath> m_maxAtOnce;
public:
    CEsdlTransformOperationSynchronize(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix, IEsdlFunctionRegister *functionRegister)
        : CEsdlTransformOperationWithChildren(xpp, stag, prefix, false, functionRegister, nullptr)
    {
        const char *maxAtOnce = stag.getValue("max-at-once");
        if (!isEmptyString(maxAtOnce))
            m_maxAtOnce.setown(compileXpath(maxAtOnce));
    }

    virtual ~CEsdlTransformOperationSynchronize(){}

    virtual bool exec(CriticalSection *externalCrit, IInterface *preparedForAsync, IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext) override
    {
        if (!m_children.length())
            return false;

        IPointerArray preps;
        ForEachItemIn(i, m_children)
            preps.append(m_children.item(i).prepareForAsync(scriptContext, targetContext, sourceContext));

        class casyncfor: public CAsyncFor
        {
            CriticalSection synchronizeCrit;
            IPointerArray &preps;
            IArrayOf<IEsdlTransformOperation> &children;
            IEsdlScriptContext *scriptContext = nullptr;
            IXpathContext *targetContext = nullptr;
            IXpathContext *sourceContext = nullptr;

        public:
            casyncfor(IPointerArray &_preps, IArrayOf<IEsdlTransformOperation> &_children, IEsdlScriptContext *_scriptContext, IXpathContext *_targetContext, IXpathContext *_sourceContext)
                : preps(_preps), children(_children), scriptContext(_scriptContext), targetContext(_targetContext), sourceContext(_sourceContext)
            {
            }
            void Do(unsigned i)
            {
                children.item(i).exec(&synchronizeCrit, preps.item(i), scriptContext, targetContext, sourceContext);
            }
        } afor(preps, m_children, scriptContext, targetContext, sourceContext);

        unsigned maxAtOnce = m_maxAtOnce.get() ? (unsigned) sourceContext->evaluateAsNumber(m_maxAtOnce) : 5;
        afor.For(m_children.ordinality(), maxAtOnce, false, false);

        return true;
    }

    virtual void toDBGLog () override
    {
    #if defined(_DEBUG)
        DBGLOG (">>>>%s ", m_tagname.str());
        CEsdlTransformOperationWithChildren::toDBGLog();
        DBGLOG ("<<<<%s<<<<<", m_tagname.str());
    #endif
    }
};

class CEsdlTransformOperationConditional : public CEsdlTransformOperationWithChildren
{
private:
    Owned<ICompiledXpath> m_test;
    char m_op = 'i'; //'i'=if, 'w'=when, 'o'=otherwise

public:
    CEsdlTransformOperationConditional(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix, IEsdlFunctionRegister *functionRegister)
        : CEsdlTransformOperationWithChildren(xpp, stag, prefix, true, functionRegister, nullptr)
    {
        const char *op = stag.getLocalName();
        if (isEmptyString(op)) //should never get here, we checked already, but
            esdlOperationError(ESDL_SCRIPT_UnknownOperation, m_tagname, "unrecognized conditional missing tag name", !m_ignoreCodingErrors);
        //m_ignoreCodingErrors means op may still be null
        else if (!op || streq(op, "if"))
            m_op = 'i';
        else if (streq(op, "when"))
            m_op = 'w';
        else if (streq(op, "otherwise"))
            m_op = 'o';
        else //should never get here either, but
            esdlOperationError(ESDL_SCRIPT_UnknownOperation, m_tagname, "unrecognized conditional tag name", !m_ignoreCodingErrors);

        if (m_op!='o')
        {
            const char *test = stag.getValue("test");
            if (isEmptyString(test))
                esdlOperationError(ESDL_SCRIPT_MissingOperationAttr, m_tagname, "without test", !m_ignoreCodingErrors);
            m_test.setown(compileXpath(test));
        }
    }

    virtual ~CEsdlTransformOperationConditional(){}

    virtual bool exec(CriticalSection *crit, IInterface *preparedForAsync, IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext) override
    {
        OptionalCriticalBlock block(crit);

        if (!evaluate(sourceContext))
            return false;
        processChildren(scriptContext, targetContext, sourceContext);
        return true; //just means that evaluation succeeded and attempted to process children
    }

    virtual void toDBGLog () override
    {
    #if defined(_DEBUG)
        DBGLOG (">>>>%s %s ", m_tagname.str(), m_test ? m_test->getXpath() : "");
        CEsdlTransformOperationWithChildren::toDBGLog();
        DBGLOG ("<<<<%s<<<<<", m_tagname.str());
    #endif
    }

private:
    bool evaluate(IXpathContext * xpathContext)
    {
        if (m_op=='o')  //'o'/"otherwise" is unconditional
            return true;
        bool match = false;
        try
        {
            match = xpathContext->evaluateAsBoolean(m_test);
        }
        catch (IException* e)
        {
            int code = e->errorCode();
            StringBuffer msg;
            e->errorMessage(msg);
            e->Release();
            esdlOperationError(code, m_tagname, msg, !m_ignoreCodingErrors);
        }
        catch (...)
        {
            VStringBuffer msg("unknown exception evaluating test '%s'", m_test.get() ? m_test->getXpath() : "undefined!");
            esdlOperationError(ESDL_SCRIPT_Error, m_tagname, msg, !m_ignoreCodingErrors);
        }
        return match;
    }
};

void loadChooseChildren(IArrayOf<IEsdlTransformOperation> &operations, IXmlPullParser &xpp, const StringBuffer &prefix, bool withVariables, bool ignoreCodingErrors, IEsdlFunctionRegister *functionRegister)
{
    Owned<CEsdlTransformOperationConditional> otherwise;

    int type = 0;
    while((type = xpp.next()) != XmlPullParser::END_DOCUMENT)
    {
        switch(type)
        {
            case XmlPullParser::START_TAG:
            {
                StartTag opTag;
                xpp.readStartTag(opTag);
                const char *op = opTag.getLocalName();
                if (streq(op, "when"))
                    operations.append(*new CEsdlTransformOperationConditional(xpp, opTag, prefix, functionRegister));
                else if (streq(op, "otherwise"))
                {
                    if (otherwise)
                        esdlOperationError(ESDL_SCRIPT_Error, op, "only 1 otherwise per choose statement allowed", ignoreCodingErrors);
                    otherwise.setown(new CEsdlTransformOperationConditional(xpp, opTag, prefix, functionRegister));
                }
                break;
            }
            case XmlPullParser::END_TAG:
            case XmlPullParser::END_DOCUMENT:
            {
                if (otherwise)
                    operations.append(*otherwise.getClear());
                return;
            }
        }
    }
}

class CEsdlTransformOperationChoose : public CEsdlTransformOperationWithChildren
{
public:
    CEsdlTransformOperationChoose(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix, IEsdlFunctionRegister *functionRegister)
        : CEsdlTransformOperationWithChildren(xpp, stag, prefix, false, functionRegister, loadChooseChildren)
    {
    }

    virtual ~CEsdlTransformOperationChoose(){}

    virtual bool exec(CriticalSection *crit, IInterface *preparedForAsync, IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext) override
    {
        OptionalCriticalBlock block(crit);

        return processChildren(scriptContext, targetContext, sourceContext);
    }

    virtual bool processChildren(IEsdlScriptContext * scriptContext, IXpathContext *targetContext, IXpathContext * sourceContext) override
    {
        if (m_children.length())
        {
            CXpathContextScope scope(sourceContext, "choose", XpathVariableScopeType::simple, nullptr);
            ForEachItemIn(i, m_children)
            {
                if (m_children.item(i).process(scriptContext, targetContext, sourceContext))
                    return true;
            }
        }
        return false;
    }

    virtual void toDBGLog () override
    {
    #if defined(_DEBUG)
        DBGLOG (">>>>>>>>>>> %s >>>>>>>>>>", m_tagname.str());
        CEsdlTransformOperationWithChildren::toDBGLog();
        DBGLOG (">>>>>>>>>>> %s >>>>>>>>>>", m_tagname.str());
    #endif
    }
};

void loadCallWithParameters(IArrayOf<IEsdlTransformOperation> &operations, IXmlPullParser &xpp, const StringBuffer &prefix, bool withVariables, bool ignoreCodingErrors, IEsdlFunctionRegister *functionRegister)
{
    int type = 0;
    while((type = xpp.next()) != XmlPullParser::END_DOCUMENT)
    {
        switch(type)
        {
            case XmlPullParser::START_TAG:
            {
                StartTag opTag;
                xpp.readStartTag(opTag);
                const char *op = opTag.getLocalName();
                if (streq(op, "with-param"))
                    operations.append(*new CEsdlTransformOperationVariable(xpp, opTag, prefix, functionRegister));
                else
                    esdlOperationError(ESDL_SCRIPT_Error, op, "Unrecognized operation, only 'with-param' allowed within 'call-function'", ignoreCodingErrors);

                break;
            }
            case XmlPullParser::END_TAG:
            case XmlPullParser::END_DOCUMENT:
                return;
        }
    }
}

class CEsdlTransformOperationCallFunction : public CEsdlTransformOperationWithChildren
{
private:
    StringAttr m_name;
    //the localFunctionRegister is used at compile time to register this object,
    // and then only for looking up functions defined inside the local script
    IEsdlFunctionRegister *localFunctionRegister = nullptr;
    IEsdlTransformOperation *esdlFunc = nullptr;

public:
    CEsdlTransformOperationCallFunction(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix, IEsdlFunctionRegister *_functionRegister)
        : CEsdlTransformOperationWithChildren(xpp, stag, prefix, false /* we need to handle variable scope below (see comment)*/, _functionRegister, loadCallWithParameters), localFunctionRegister(_functionRegister)
    {
        m_name.set(stag.getValue("name"));
        if (m_name.isEmpty())
            esdlOperationError(ESDL_SCRIPT_MissingOperationAttr, "call-function", "without name parameter", m_traceName.str(), !m_ignoreCodingErrors);
        localFunctionRegister->registerEsdlFunctionCall(this);
    }
    virtual ~CEsdlTransformOperationCallFunction()
    {
    }
    void bindFunctionCall(const char *scopeDescr, IEsdlFunctionRegister *activeFunctionRegister, bool bindLocalOnly)
    {
        //we always use / cache function pointer defined local to current script
        esdlFunc = localFunctionRegister->findEsdlFunction(m_name, true);
        if (!esdlFunc)
        {
            //the activeFunctionRegister is associated with the ESDL method currently being bound
            IEsdlTransformOperation *foundFunc = activeFunctionRegister->findEsdlFunction(m_name, false);
            if (foundFunc)
            {
                if (!bindLocalOnly)
                    esdlFunc = foundFunc;
            }
            else
            {
                //if bindLocalOnly, it's just a warning if we didn't cache the function pointer
                //  this is intended for function calls in service level scripts which will be looked up at runtime if they aren't local to the script
                VStringBuffer msg("function (%s) not found for %s", m_name.str(), scopeDescr);
                if (bindLocalOnly)
                    esdlOperationWarning(ESDL_SCRIPT_Warning, "call-function", msg.str(), m_traceName.str());
                else
                    esdlOperationError(ESDL_SCRIPT_Error, "call-function", msg.str(), m_traceName.str(), true);
            }
        }
    }

    virtual bool exec(CriticalSection *crit, IInterface *preparedForAsync, IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext) override
    {
        //we should only get here with esdlFunc==nullptr for calls made in service level scripts where the function
        //  isn't defined locally to the script
        IEsdlTransformOperation *callFunc = esdlFunc;
        if (!callFunc)
        {
            IEsdlFunctionRegister *activeRegister = static_cast<IEsdlFunctionRegister*>(scriptContext->queryFunctionRegister());
            if (!activeRegister)
                throw MakeStringException(ESDL_SCRIPT_Error, "Runtime function register not found (looking up %s)", m_name.str());
            callFunc = activeRegister->findEsdlFunction(m_name, false);
            if (!callFunc)
                throw MakeStringException(ESDL_SCRIPT_Error, "Function (%s) not found (runtime)", m_name.str());
        }

        //Can't have CEsdlTransformOperationWithChildren create the scope, it would be destroyed before esdlFunc->process() was called below
        Owned<CXpathContextScope> scope = new CXpathContextScope(sourceContext, m_tagname, XpathVariableScopeType::parameter, nullptr);

        //in this case, processing children is setting up parameters for the function call that follows
        processChildren(scriptContext, targetContext, sourceContext);

        return callFunc->process(scriptContext, targetContext, sourceContext);
    }

    virtual void toDBGLog () override
    {
    #if defined(_DEBUG)
        DBGLOG (">>>>>>>>>>> %s %s >>>>>>>>>>", m_tagname.str(), m_name.str());
        CEsdlTransformOperationWithChildren::toDBGLog();
        DBGLOG (">>>>>>>>>>> %s >>>>>>>>>>", m_tagname.str());
    #endif
    }
};

class CEsdlTransformOperationTarget : public CEsdlTransformOperationWithChildren
{
protected:
    Owned<ICompiledXpath> m_xpath;
    bool m_required = true;
    bool m_ensure = false;

public:
    CEsdlTransformOperationTarget(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix, IEsdlFunctionRegister *functionRegister)
        : CEsdlTransformOperationWithChildren(xpp, stag, prefix, true, functionRegister, nullptr)
    {
        const char *xpath = stag.getValue("xpath");
        if (isEmptyString(xpath))
            esdlOperationError(ESDL_SCRIPT_MissingOperationAttr, "target", "without xpath parameter", m_traceName.str(), !m_ignoreCodingErrors);

        m_xpath.setown(compileXpath(xpath));
        m_required = getStartTagValueBool(stag, "required", m_required);
    }

    virtual ~CEsdlTransformOperationTarget(){}

    virtual bool exec(CriticalSection *crit, IInterface *preparedForAsync, IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext) override
    {
        OptionalCriticalBlock block(crit);

        CXpathContextLocation location(targetContext);
        bool success = false;
        if (m_ensure)
            success = targetContext->ensureLocation(m_xpath->getXpath(), m_required);
        else
            success = targetContext->setLocation(m_xpath, m_required);

        if (success)
            return processChildren(scriptContext, targetContext, sourceContext);
        return false;
    }

    virtual void toDBGLog () override
    {
    #if defined(_DEBUG)
        DBGLOG(">>>%s> %s(%s)>>>>", m_traceName.str(), m_tagname.str(), m_xpath.get() ? m_xpath->getXpath() : "");
        CEsdlTransformOperationWithChildren::toDBGLog();
        DBGLOG (">>>>>>>>>>> %s >>>>>>>>>>", m_tagname.str());
    #endif
    }
};

//the script element serves as a simple wrapper around a section of script content
//  the initial use to put a script section child within a synchronize element, providing a section of script to run while other synchronize children are blocked
//but script is supported anywhere and might be useful elsewhere for providing some scope for variables and for structuring code
class CEsdlTransformOperationScript : public CEsdlTransformOperationWithChildren
{
public:
    CEsdlTransformOperationScript(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix, IEsdlFunctionRegister *functionRegister)
        : CEsdlTransformOperationWithChildren(xpp, stag, prefix, true, functionRegister, nullptr)
    {
    }

    virtual ~CEsdlTransformOperationScript(){}

    virtual bool exec(CriticalSection *crit, IInterface *preparedForAsync, IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext) override
    {
        OptionalCriticalBlock block(crit);
        return processChildren(scriptContext, targetContext, sourceContext);
    }

    virtual void toDBGLog () override
    {
    #if defined(_DEBUG)
        DBGLOG(">>>%s> %s>>>>", m_traceName.str(), m_tagname.str());
        CEsdlTransformOperationWithChildren::toDBGLog();
        DBGLOG (">>>>>>>>>>> %s >>>>>>>>>>", m_tagname.str());
    #endif
    }
};

class CEsdlTransformOperationIfTarget : public CEsdlTransformOperationTarget
{
public:
    CEsdlTransformOperationIfTarget(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix, IEsdlFunctionRegister *functionRegister)
        : CEsdlTransformOperationTarget(xpp, stag, prefix, functionRegister)
    {
        m_required = false;
    }

    virtual ~CEsdlTransformOperationIfTarget(){}
};

class CEsdlTransformOperationEnsureTarget : public CEsdlTransformOperationTarget
{
public:
    CEsdlTransformOperationEnsureTarget(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix, IEsdlFunctionRegister *functionRegister)
        : CEsdlTransformOperationTarget(xpp, stag, prefix, functionRegister)
    {
        m_ensure = true;
    }

    virtual ~CEsdlTransformOperationEnsureTarget(){}
};

class CEsdlTransformOperationSource : public CEsdlTransformOperationWithChildren
{
protected:
    Owned<ICompiledXpath> m_xpath;
    bool m_required = true;

public:
    CEsdlTransformOperationSource(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix, IEsdlFunctionRegister *functionRegister)
        : CEsdlTransformOperationWithChildren(xpp, stag, prefix, true, functionRegister, nullptr)
    {
        const char *xpath = stag.getValue("xpath");
        if (isEmptyString(xpath))
            esdlOperationError(ESDL_SCRIPT_MissingOperationAttr, "target", "without xpath parameter", m_traceName.str(), !m_ignoreCodingErrors);

        m_xpath.setown(compileXpath(xpath));
        m_required = getStartTagValueBool(stag, "required", m_required);
    }

    virtual ~CEsdlTransformOperationSource(){}

    virtual bool exec(CriticalSection *crit, IInterface *preparedForAsync, IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext) override
    {
        OptionalCriticalBlock block(crit);

        CXpathContextLocation location(sourceContext);
        if (sourceContext->setLocation(m_xpath, m_required))
            return processChildren(scriptContext, targetContext, sourceContext);
        return false;
    }

    virtual void toDBGLog () override
    {
    #if defined(_DEBUG)
        DBGLOG(">>>%s> %s(%s)>>>>", m_traceName.str(), m_tagname.str(), m_xpath.get() ? m_xpath->getXpath() : "");
        CEsdlTransformOperationWithChildren::toDBGLog();
        DBGLOG (">>>>>>>>>>> %s >>>>>>>>>>", m_tagname.str());
    #endif
    }
};

class CEsdlTransformOperationIfSource : public CEsdlTransformOperationSource
{
public:
    CEsdlTransformOperationIfSource(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix, IEsdlFunctionRegister *functionRegister)
        : CEsdlTransformOperationSource(xpp, stag, prefix, functionRegister)
    {
        m_required = false;
    }

    virtual ~CEsdlTransformOperationIfSource(){}
};


class CEsdlTransformOperationElement : public CEsdlTransformOperationWithChildren
{
protected:
    StringBuffer m_name;
    StringBuffer m_nsuri;

public:
    CEsdlTransformOperationElement(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix, IEsdlFunctionRegister *functionRegister)
        : CEsdlTransformOperationWithChildren(xpp, stag, prefix, true, functionRegister, nullptr)
    {
        m_name.set(stag.getValue("name"));
        if (m_name.isEmpty())
            esdlOperationError(ESDL_SCRIPT_MissingOperationAttr, "element", "without name parameter", m_traceName.str(), !m_ignoreCodingErrors);
        if (m_traceName.isEmpty())
            m_traceName.set(m_name);

        if (!validateXMLTag(m_name))
        {
            VStringBuffer msg("with invalid element name '%s'", m_name.str());
            esdlOperationError(ESDL_SCRIPT_MissingOperationAttr, "element", msg.str(), m_traceName.str(), !m_ignoreCodingErrors);
        }

        m_nsuri.set(stag.getValue("namespace"));
    }

    virtual ~CEsdlTransformOperationElement(){}

    virtual bool exec(CriticalSection *crit, IInterface *preparedForAsync, IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext) override
    {
        OptionalCriticalBlock block(crit);

        CXpathContextLocation location(targetContext);
        targetContext->addElementToLocation(m_name);
        return processChildren(scriptContext, targetContext, sourceContext);
    }

    virtual void toDBGLog () override
    {
    #if defined(_DEBUG)
        DBGLOG (">>>>>>>>>>> %s (%s, nsuri(%s)) >>>>>>>>>>", m_tagname.str(), m_name.str(), m_nsuri.str());
        CEsdlTransformOperationWithChildren::toDBGLog();
        DBGLOG (">>>>>>>>>>> %s >>>>>>>>>>", m_tagname.str());
    #endif
    }
};

void createEsdlTransformOperations(IArrayOf<IEsdlTransformOperation> &operations, IXmlPullParser &xpp, const StringBuffer &prefix, bool withVariables, bool ignoreCodingErrors, IEsdlFunctionRegister *functionRegister)
{
    int type = 0;
    while((type = xpp.next()) != XmlPullParser::END_DOCUMENT)
    {
        switch(type)
        {
            case XmlPullParser::START_TAG:
            {
                Owned<IEsdlTransformOperation> operation = createEsdlTransformOperation(xpp, prefix, withVariables, ignoreCodingErrors, functionRegister, false);
                if (operation)
                    operations.append(*operation.getClear());
                break;
            }
            case XmlPullParser::END_TAG:
                return;
            case XmlPullParser::END_DOCUMENT:
                return;
        }
    }
}

class CEsdlTransformOperationFunction : public CEsdlTransformOperationWithChildren
{
public:
    StringAttr m_name;

public:
    CEsdlTransformOperationFunction(IXmlPullParser &xpp, StartTag &stag, const StringBuffer &prefix, IEsdlFunctionRegister *functionRegister)
        : CEsdlTransformOperationWithChildren(xpp, stag, prefix, true, functionRegister, nullptr)
    {
        m_name.set(stag.getValue("name"));
        if (m_name.isEmpty())
            esdlOperationError(ESDL_SCRIPT_MissingOperationAttr, "function", "without name parameter", m_traceName.str(), !m_ignoreCodingErrors);
        m_childScopeType = XpathVariableScopeType::isolated;
    }

    virtual ~CEsdlTransformOperationFunction(){}

    virtual bool exec(CriticalSection *crit, IInterface *preparedForAsync, IEsdlScriptContext * scriptContext, IXpathContext * targetContext, IXpathContext * sourceContext) override
    {
        return processChildren(scriptContext, targetContext, sourceContext);
    }

    virtual void toDBGLog () override
    {
    #if defined(_DEBUG)
        DBGLOG(">>>%s> %s %s >>>>", m_traceName.str(), m_tagname.str(), m_name.str());
        CEsdlTransformOperationWithChildren::toDBGLog();
        DBGLOG (">>>>>>>>>>> %s >>>>>>>>>>", m_tagname.str());
    #endif
    }
};

IEsdlTransformOperation *createEsdlTransformOperation(IXmlPullParser &xpp, const StringBuffer &prefix, bool withVariables, bool ignoreCodingErrors, IEsdlFunctionRegister *functionRegister, bool canDeclareFunctions)
{
    StartTag stag;
    xpp.readStartTag(stag);
    const char *op = stag.getLocalName();
    if (isEmptyString(op))
        return nullptr;
    if (functionRegister)
    {
        if (streq(op, "function"))
        {
            if (!canDeclareFunctions)
                esdlOperationError(ESDL_SCRIPT_Error, "function", "can only declare functions at root level", !ignoreCodingErrors);
            Owned<CEsdlTransformOperationFunction> esdlFunc = new CEsdlTransformOperationFunction(xpp, stag, prefix, functionRegister);
            functionRegister->registerEsdlFunction(esdlFunc->m_name.str(), static_cast<IEsdlTransformOperation*>(esdlFunc.get()));
            return nullptr;
        }
    }
    if (withVariables)
    {
        if (streq(op, "variable"))
            return new CEsdlTransformOperationVariable(xpp, stag, prefix, functionRegister);
        if (streq(op, "param"))
            return new CEsdlTransformOperationParameter(xpp, stag, prefix, functionRegister);
    }
    if (streq(op, "choose"))
        return new CEsdlTransformOperationChoose(xpp, stag, prefix, functionRegister);
    if (streq(op, "for-each"))
        return new CEsdlTransformOperationForEach(xpp, stag, prefix, functionRegister);
    if (streq(op, "if"))
        return new CEsdlTransformOperationConditional(xpp, stag, prefix, functionRegister);
    if (streq(op, "set-value") || streq(op, "SetValue"))
        return new CEsdlTransformOperationSetValue(xpp, stag, prefix);
    if (streq(op, "append-to-value") || streq(op, "AppendValue"))
        return new CEsdlTransformOperationAppendValue(xpp, stag, prefix);
    if (streq(op, "add-value"))
        return new CEsdlTransformOperationAddValue(xpp, stag, prefix);
    if (streq(op, "fail"))
        return new CEsdlTransformOperationFail(xpp, stag, prefix);
    if (streq(op, "assert"))
        return new CEsdlTransformOperationAssert(xpp, stag, prefix);
    if (streq(op, "store-value"))
        return new CEsdlTransformOperationStoreValue(xpp, stag, prefix);
    if (streq(op, "set-log-profile"))
        return new CEsdlTransformOperationSetLogProfile(xpp, stag, prefix);
    if (streq(op, "set-log-option"))
        return new CEsdlTransformOperationSetLogOption(xpp, stag, prefix);
    if (streq(op, "rename-node"))
        return new CEsdlTransformOperationRenameNode(xpp, stag, prefix);
    if (streq(op, "remove-node"))
        return new CEsdlTransformOperationRemoveNode(xpp, stag, prefix);
    if (streq(op, "source"))
        return new CEsdlTransformOperationSource(xpp, stag, prefix, functionRegister);
    if (streq(op, "if-source"))
        return new CEsdlTransformOperationIfSource(xpp, stag, prefix, functionRegister);
    if (streq(op, "target"))
        return new CEsdlTransformOperationTarget(xpp, stag, prefix, functionRegister);
    if (streq(op, "if-target"))
        return new CEsdlTransformOperationIfTarget(xpp, stag, prefix, functionRegister);
    if (streq(op, "ensure-target"))
        return new CEsdlTransformOperationEnsureTarget(xpp, stag, prefix, functionRegister);
    if (streq(op, "element"))
        return new CEsdlTransformOperationElement(xpp, stag, prefix, functionRegister);
    if (streq(op, "copy-of"))
        return new CEsdlTransformOperationCopyOf(xpp, stag, prefix);
    if (streq(op, "namespace"))
        return new CEsdlTransformOperationNamespace(xpp, stag, prefix);
    if (streq(op, "http-post-xml"))
        return new CEsdlTransformOperationHttpPostXml(xpp, stag, prefix, functionRegister);
    if (streq(op, "mysql"))
        return new CEsdlTransformOperationMySqlCall(xpp, stag, prefix);
    if (streq(op, "trace"))
        return new CEsdlTransformOperationTrace(xpp, stag, prefix);
    if (streq(op, "call-function"))
        return new CEsdlTransformOperationCallFunction(xpp, stag, prefix, functionRegister);
    if (streq(op, "synchronize"))
        return new CEsdlTransformOperationSynchronize(xpp, stag, prefix, functionRegister);
    if (streq(op, "script"))
        return new CEsdlTransformOperationScript(xpp, stag, prefix, functionRegister);
    return nullptr;
}

static inline void replaceVariable(StringBuffer &s, IXpathContext *xpathContext, const char *name)
{
    StringBuffer temp;
    const char *val = xpathContext->getVariable(name, temp);
    if (val)
    {
        VStringBuffer match("{$%s}", name);
        s.replaceString(match, val);
    }
}

class CEsdlFunctionRegister : public CInterfaceOf<IEsdlFunctionRegister>
{
private:
    CEsdlFunctionRegister *parent = nullptr; //usual hierarchy of function registries is: service / method / script
    MapStringToMyClass<IEsdlTransformOperation> functions;
    IArrayOf<CEsdlTransformOperationCallFunction> functionCalls;

    //noLocalFunctions
    // - true if we are in an entry point of the type "functions".. adding functions to the service or method scopes, not locally.
    // - false if we are defining a function for local use within an idividual script.
    bool noLocalFunctions = false;

public:
    CEsdlFunctionRegister(CEsdlFunctionRegister *_parent, bool _noLocalFunctions) : parent(_parent), noLocalFunctions(_noLocalFunctions)
    {
        if (noLocalFunctions)
           assertex(parent); //should never happen
    }
    virtual void registerEsdlFunction(const char *name, IEsdlTransformOperation *esdlFunc) override
    {
        if (noLocalFunctions) //register with parent, not locally
            parent->registerEsdlFunction(name, esdlFunc);
        else
            functions.setValue(name, esdlFunc);
    }
    virtual IEsdlTransformOperation *findEsdlFunction(const char *name, bool localOnly) override
    {
        IEsdlTransformOperation *esdlFunc =  functions.getValue(name);
        if (!esdlFunc && !localOnly && parent)
            return parent->findEsdlFunction(name, false);
        return esdlFunc;
    }
    virtual void registerEsdlFunctionCall(IEsdlTransformOperation *esdlFuncCall) override
    {
        //in the case of call-function, if noLocalFunctions is true we are a call-function inside another function that is not local inside a script
        //  so same registration semantics apply
        if (noLocalFunctions) //register with parent, not locally
            parent->registerEsdlFunctionCall(esdlFuncCall);
        else
            functionCalls.append(*LINK(static_cast<CEsdlTransformOperationCallFunction*>(esdlFuncCall)));
    }
    void bindFunctionCalls(const char *scopeDescr, IEsdlFunctionRegister *activeFunctionRegister, bool bindLocalOnly)
    {
        ForEachItemIn(idx, functionCalls)
            functionCalls.item(idx).bindFunctionCall(scopeDescr, activeFunctionRegister, bindLocalOnly);
    }
};

class CEsdlCustomTransform : public CInterfaceOf<IEsdlCustomTransform>
{
private:
    IArrayOf<IEsdlTransformOperation> m_operations;
    CEsdlFunctionRegister functionRegister;

    Owned<IProperties> namespaces = createProperties(false);
    StringAttr m_name;
    StringAttr m_target;
    StringAttr m_source;
    StringBuffer m_prefix;

public:
    CEsdlCustomTransform(CEsdlFunctionRegister *parentRegister, bool loadingCommonFunctions)
        : functionRegister(parentRegister, loadingCommonFunctions) {}

    CEsdlCustomTransform(IXmlPullParser &xpp, StartTag &stag, const char *ns_prefix, CEsdlFunctionRegister *parentRegister, bool loadingCommonFunctions)
        : functionRegister(parentRegister, loadingCommonFunctions), m_prefix(ns_prefix)
    {
        const char *tag = stag.getLocalName();

        m_name.set(stag.getValue("name"));
        m_target.set(stag.getValue("target"));
        m_source.set(stag.getValue("source"));

        DBGLOG("Compiling ESDL Transform: '%s'", m_name.str());

        map< string, const SXT_CHAR* >::const_iterator it = xpp.getNsBegin();
        while (it != xpp.getNsEnd())
        {
            if (it->first.compare("xml")!=0)
                namespaces->setProp(it->first.c_str(), it->second);
            it++;
        }

        int type = 0;
        while((type = xpp.next()) != XmlPullParser::END_DOCUMENT)
        {
            switch(type)
            {
                case XmlPullParser::START_TAG:
                {
                    Owned<IEsdlTransformOperation> operation = createEsdlTransformOperation(xpp, m_prefix, true, false, &functionRegister, true);
                    if (operation)
                        m_operations.append(*operation.getClear());
                    break;
                }
                case XmlPullParser::END_TAG:
                case XmlPullParser::END_DOCUMENT:
                    return;
            }
        }
    }

    virtual void appendPrefixes(StringArray &prefixes) override
    {
        if (m_prefix.length())
        {
            StringAttr copy(m_prefix.str(), m_prefix.length()-1); //remove the colon
            prefixes.appendUniq(copy.str());
        }
        else
            prefixes.appendUniq("");
    }
    virtual void toDBGLog() override
    {
#if defined(_DEBUG)
        DBGLOG(">>>>>>>>>>>>>>>>transform: '%s'>>>>>>>>>>", m_name.str());
        ForEachItemIn(i, m_operations)
            m_operations.item(i).toDBGLog();
        DBGLOG("<<<<<<<<<<<<<<<<transform<<<<<<<<<<<<");
#endif
     }

    virtual ~CEsdlCustomTransform(){}

    void processTransformImpl(IEsdlScriptContext * scriptContext, const char *srcSection, const char *tgtSection, IXpathContext *sourceContext, const char *target) override
    {
        if (m_target.length())
            target = m_target.str();
        Owned<IXpathContext> targetXpath = nullptr;
        if (isEmptyString(tgtSection))
            targetXpath.setown(scriptContext->createXpathContext(sourceContext, srcSection, true));
        else
            targetXpath.setown(scriptContext->getCopiedSectionXpathContext(sourceContext, tgtSection, srcSection, true));

        Owned<IProperties> savedNamespaces = createProperties(false);
        Owned<IPropertyIterator> ns = namespaces->getIterator();
        ForEach(*ns)
        {
            const char *prefix = ns->getPropKey();
            const char *existing = sourceContext->queryNamespace(prefix);
            savedNamespaces->setProp(prefix, isEmptyString(existing) ? "" : existing);
            sourceContext->registerNamespace(prefix, namespaces->queryProp(prefix));
            targetXpath->registerNamespace(prefix, namespaces->queryProp(prefix));
        }
        CXpathContextScope scope(sourceContext, "transform", XpathVariableScopeType::simple, savedNamespaces);
        if (!isEmptyString(target) && !streq(target, "."))
            targetXpath->setLocation(target, true);
        if (!m_source.isEmpty() && !streq(m_source, "."))
            sourceContext->setLocation(m_source, true);
        ForEachItemIn(i, m_operations)
            m_operations.item(i).process(scriptContext, targetXpath, sourceContext);
        scriptContext->cleanupBetweenScripts();
    }
    void bindFunctionCalls(const char *scopeDescr, IEsdlFunctionRegister *activeFunctionRegister, bool bindLocalOnly)
    {
        functionRegister.bindFunctionCalls(scopeDescr, activeFunctionRegister, bindLocalOnly);
    }

    void processTransform(IEsdlScriptContext * scriptCtx, const char *srcSection, const char *tgtSection) override;
};

class CEsdlCustomTransformWrapper : public CInterfaceOf<IEsdlTransformSet>
{
    Linked<CEsdlCustomTransform> crt;
public:
    CEsdlCustomTransformWrapper(CEsdlCustomTransform *t) : crt(t) {}
    void processTransformImpl(IEsdlScriptContext * context, const char *srcSection, const char *tgtSection, IXpathContext *sourceContext, const char *target) override
    {
        crt->processTransformImpl(context, srcSection, tgtSection, sourceContext, target);
    }
    void appendPrefixes(StringArray &prefixes) override
    {
        crt->appendPrefixes(prefixes);
    }
    aindex_t length() override
    {
        return crt ? 1 : 0;
    }
};

void CEsdlCustomTransform::processTransform(IEsdlScriptContext * scriptCtx, const char *srcSection, const char *tgtSection)
{
    CEsdlCustomTransformWrapper tfw(this);
    processServiceAndMethodTransforms(scriptCtx, {static_cast<IEsdlTransformSet*>(&tfw)}, srcSection, tgtSection);
}

void processServiceAndMethodTransforms(IEsdlScriptContext * scriptCtx, std::initializer_list<IEsdlTransformSet *> const &transforms, const char *srcSection, const char *tgtSection)
{
    LogLevel level = LogMin;
    if (!scriptCtx)
        return;
    if (!transforms.size())
        return;
    if (isEmptyString(srcSection))
    {
      if (!isEmptyString(tgtSection))
        return;
    }
    level = (LogLevel) scriptCtx->getXPathInt64("target/*/@traceLevel", level);

    const char *method = scriptCtx->queryAttribute(ESDLScriptCtxSection_ESDLInfo, "method");
    if (isEmptyString(method))
        throw MakeStringException(ESDL_SCRIPT_Error, "ESDL script method name not set");
    const char *service = scriptCtx->queryAttribute(ESDLScriptCtxSection_ESDLInfo, "service");
    if (isEmptyString(service))
        throw MakeStringException(ESDL_SCRIPT_Error, "ESDL script service name not set");
    const char *reqtype = scriptCtx->queryAttribute(ESDLScriptCtxSection_ESDLInfo, "request_type");
    if (isEmptyString(reqtype))
        throw MakeStringException(ESDL_SCRIPT_Error, "ESDL script request name not set");

    IEspContext *context = reinterpret_cast<IEspContext*>(scriptCtx->queryEspContext());

    if (level >= LogMax)
    {
        StringBuffer logtxt;
        scriptCtx->toXML(logtxt, srcSection, false);
        DBGLOG("ORIGINAL content: %s", logtxt.str());
        scriptCtx->toXML(logtxt.clear(), ESDLScriptCtxSection_BindingConfig);
        DBGLOG("BINDING CONFIG: %s", logtxt.str());
        scriptCtx->toXML(logtxt.clear(), ESDLScriptCtxSection_TargetConfig);
        DBGLOG("TARGET CONFIG: %s", logtxt.str());
    }

    bool strictParams = scriptCtx->getXPathBool("config/*/@strictParams", false);
    Owned<IXpathContext> sourceContext = scriptCtx->createXpathContext(nullptr, srcSection, strictParams);

    StringArray prefixes;
    for ( IEsdlTransformSet * const & item : transforms)
    {
        if (item)
            item->appendPrefixes(prefixes);
    }

    registerEsdlXPathExtensions(sourceContext, scriptCtx, prefixes);

    VStringBuffer ver("%g", context->getClientVersion());
    if(!sourceContext->addVariable("clientversion", ver.str()))
        OERRLOG("Could not set ESDL Script variable: clientversion:'%s'", ver.str());

    //in case transform wants to make use of these values:
    //make them few well known values variables rather than inputs so they are automatically available
    StringBuffer temp;
    sourceContext->addVariable("query", scriptCtx->getXPathString("target/*/@queryname", temp));

    ISecUser *user = context->queryUser();
    if (user)
    {
        static const std::map<SecUserStatus, const char*> statusLabels =
        {
#define STATUS_LABEL_NODE(s) { s, #s }
            STATUS_LABEL_NODE(SecUserStatus_Inhouse),
            STATUS_LABEL_NODE(SecUserStatus_Active),
            STATUS_LABEL_NODE(SecUserStatus_Exempt),
            STATUS_LABEL_NODE(SecUserStatus_FreeTrial),
            STATUS_LABEL_NODE(SecUserStatus_csdemo),
            STATUS_LABEL_NODE(SecUserStatus_Rollover),
            STATUS_LABEL_NODE(SecUserStatus_Suspended),
            STATUS_LABEL_NODE(SecUserStatus_Terminated),
            STATUS_LABEL_NODE(SecUserStatus_TrialExpired),
            STATUS_LABEL_NODE(SecUserStatus_Status_Hold),
            STATUS_LABEL_NODE(SecUserStatus_Unknown),
#undef STATUS_LABEL_NODE
        };

        Owned<IPropertyIterator> userPropIt = user->getPropertyIterator();
        ForEach(*userPropIt)
        {
            const char *name = userPropIt->getPropKey();
            if (name && *name)
                sourceContext->addInputValue(name, user->getProperty(name));
        }

        auto it = statusLabels.find(user->getStatus());

        sourceContext->addInputValue("espTransactionID", context->queryTransactionID());
        sourceContext->addInputValue("espUserName", user->getName());
        sourceContext->addInputValue("espUserRealm", user->getRealm() ? user->getRealm() : "");
        sourceContext->addInputValue("espUserPeer", user->getPeer() ? user->getPeer() : "");
        sourceContext->addInputValue("espUserStatus", VStringBuffer("%d", int(user->getStatus())));
        if (it != statusLabels.end())
            sourceContext->addInputValue("espUserStatusString", it->second);
        else
            throw MakeStringException(ESDL_SCRIPT_Error, "encountered unexpected secure user status (%d) while processing transform", int(user->getStatus()));
    }
    else
    {
        // enable transforms to distinguish secure versus insecure requests
        sourceContext->addInputValue("espTransactionID", "");
        sourceContext->addInputValue("espUserName", "");
        sourceContext->addInputValue("espUserRealm", "");
        sourceContext->addInputValue("espUserPeer", "");
        sourceContext->addInputValue("espUserStatus", "");
        sourceContext->addInputValue("espUserStatusString", "");
    }

    StringBuffer defaultTarget; //This default gives us backward compatibility with only being able to write to the actual request
    StringBuffer queryName;
    const char *tgtQueryName = scriptCtx->getXPathString("target/*/@queryname", queryName);
    if (!isEmptyString(srcSection) && streq(srcSection, ESDLScriptCtxSection_ESDLRequest))
        defaultTarget.setf("soap:Body/%s/%s", tgtQueryName ? tgtQueryName : method, reqtype);

    for ( auto&& item : transforms)
    {
        if (item)
        {
            item->processTransformImpl(scriptCtx, srcSection, tgtSection, sourceContext, defaultTarget);
        }
    }

    if (level >= LogMax)
    {
        StringBuffer content;
        scriptCtx->toXML(content);
        DBGLOG(1,"Entire script context after transforms: %s", content.str());
    }
}

IEsdlCustomTransform *createEsdlCustomTransform(const char *scriptXml, const char *ns_prefix)
{
    if (isEmptyString(scriptXml))
        return nullptr;
    std::unique_ptr<fxpp::IFragmentedXmlPullParser> xpp(fxpp::createParser());
    int bufSize = strlen(scriptXml);
    xpp->setSupportNamespaces(true);
    xpp->setInput(scriptXml, bufSize);

    int type;
    StartTag stag;
    EndTag etag;


    while((type = xpp->next()) != XmlPullParser::END_DOCUMENT)
    {
        if(type == XmlPullParser::START_TAG)
        {
            StartTag stag;
            xpp->readStartTag(stag);
            if (strieq(stag.getLocalName(), "Transforms")) //allow common mistake,.. starting with the outer tag, not the script
                continue;
            return new CEsdlCustomTransform(*xpp, stag, ns_prefix, nullptr, false);
        }
    }
    return nullptr;
}

class CEsdlTransformSet : public CInterfaceOf<IEsdlTransformSet>
{
    IArrayOf<CEsdlCustomTransform> transforms;
    CEsdlFunctionRegister *functions = nullptr;

public:
    CEsdlTransformSet(CEsdlFunctionRegister *_functions) : functions(_functions)
    {
    }
    virtual void appendPrefixes(StringArray &prefixes) override
    {
        ForEachItemIn(i, transforms)
            transforms.item(i).appendPrefixes(prefixes);
    }

    virtual void processTransformImpl(IEsdlScriptContext * scriptContext, const char *srcSection, const char *tgtSection, IXpathContext *sourceContext, const char *target) override
    {
        ForEachItemIn(i, transforms)
            transforms.item(i).processTransformImpl(scriptContext, srcSection, tgtSection, sourceContext, target);
    }
    virtual void add(IXmlPullParser &xpp, StartTag &stag)
    {
        transforms.append(*new CEsdlCustomTransform(xpp, stag, nullptr, functions, false));
    }
    virtual aindex_t length() override
    {
        return transforms.length();
    }
    void bindFunctionCalls(const char *scopeDescr, IEsdlFunctionRegister *activeFunctionRegister, bool bindLocalOnly)
    {
        ForEachItemIn(idx, transforms)
            transforms.item(idx).bindFunctionCalls(scopeDescr, activeFunctionRegister, bindLocalOnly);
    }

};

class CEsdlTransformEntryPointMap : public CInterfaceOf<IEsdlTransformEntryPointMap>
{
    MapStringToMyClass<CEsdlTransformSet> map;
    CEsdlFunctionRegister functionRegister;

public:
    CEsdlTransformEntryPointMap(CEsdlFunctionRegister *parentRegister) : functionRegister(parentRegister, false)
    {
    }

    ~CEsdlTransformEntryPointMap(){}

    virtual IEsdlFunctionRegister *queryFunctionRegister() override
    {
        return &functionRegister;
    }

    void addFunctions(IXmlPullParser &xpp, StartTag &esdlFuncTag)
    {
        //child functions will be loaded directly into the common register, container class is then no longer needed
        CEsdlCustomTransform factory(xpp, esdlFuncTag, nullptr, &functionRegister, true);
    }

    virtual void addChild(IXmlPullParser &xpp, StartTag &childTag, bool &foundNonLegacyTransforms)
    {
        const char *tagname = childTag.getLocalName();
        if (streq("Scripts", tagname) || streq("Transforms", tagname)) //allow nesting of root structure
            add(xpp, childTag, foundNonLegacyTransforms);
        else if (streq(tagname, ESDLScriptEntryPoint_Functions))
            addFunctions(xpp, childTag);
        else
        {
            if (streq(tagname, ESDLScriptEntryPoint_Legacy))
                tagname = ESDLScriptEntryPoint_BackendRequest;
            else
                foundNonLegacyTransforms = true;
            CEsdlTransformSet *set = map.getValue(tagname);
            if (set)
                set->add(xpp, childTag);
            else
            {
                Owned<CEsdlTransformSet> set = new CEsdlTransformSet(&functionRegister);
                map.setValue(tagname, set);
                set->add(xpp, childTag);
            }
        }
    }

    virtual void add(IXmlPullParser &xpp, StartTag &stag, bool &foundNonLegacyTransforms)
    {
        int type;
        StartTag childTag;
        while((type = xpp.next()) != XmlPullParser::END_DOCUMENT)
        {
            switch (type)
            {
                case XmlPullParser::START_TAG:
                {
                    xpp.readStartTag(childTag);
                    const char *tagname = childTag.getLocalName();
                    if (streq("Scripts", tagname) || streq("Transforms", tagname)) //allow nesting of container structures for maximum compatability
                        add(xpp, childTag, foundNonLegacyTransforms);
                    else
                        addChild(xpp, childTag,foundNonLegacyTransforms);
                    break;
                }
                case XmlPullParser::END_TAG:
                    return;
            }
        }
    }
    void add(const char *scriptXml, bool &foundNonLegacyTransforms)
    {
        if (isEmptyString(scriptXml))
            return;
        std::unique_ptr<XmlPullParser> xpp(new XmlPullParser());
        int bufSize = strlen(scriptXml);
        xpp->setSupportNamespaces(true);
        xpp->setInput(scriptXml, bufSize);

        int type;
        StartTag stag;
        while((type = xpp->next()) != XmlPullParser::END_DOCUMENT)
        {
            switch (type)
            {
                case XmlPullParser::START_TAG:
                {
                    xpp->readStartTag(stag);
                    addChild(*xpp, stag, foundNonLegacyTransforms);
                    break;
                }
            }
        }
    }

    virtual IEsdlTransformSet *queryEntryPoint(const char *name) override
    {
        return map.getValue(name);
    }
    virtual void removeEntryPoint(const char *name) override
    {
        map.remove(name);
    }
    void bindFunctionCalls(const char *scopeDescr, IEsdlFunctionRegister *activeFunctionRegister, bool bindLocalOnly)
    {
        functionRegister.bindFunctionCalls(scopeDescr, activeFunctionRegister, bindLocalOnly);
        HashIterator it(map);
        ForEach (it)
        {
            CEsdlTransformSet *item = map.getValue((const char *)it.query().getKey());
            if (item)
                item->bindFunctionCalls(scopeDescr, activeFunctionRegister, bindLocalOnly);
        }
    }
};

class CEsdlTransformMethodMap : public CInterfaceOf<IEsdlTransformMethodMap>
{
    MapStringToMyClass<CEsdlTransformEntryPointMap> map;
    Owned<CEsdlTransformEntryPointMap> service;
    IEsdlFunctionRegister *serviceFunctionRegister = nullptr;

public:
    CEsdlTransformMethodMap()
    {
        //ensure the service entry (name = "") exists right away, the function hierarchy depends on it
        service.setown(new CEsdlTransformEntryPointMap(nullptr));
        serviceFunctionRegister = service->queryFunctionRegister();
        map.setValue("", service.get());
    }
    virtual IEsdlTransformEntryPointMap *queryMethod(const char *name) override
    {
        return map.getValue(name);
    }
    virtual IEsdlFunctionRegister *queryFunctionRegister(const char *method) override
    {
        IEsdlTransformEntryPointMap *methodEntry = queryMethod(method);
        return (methodEntry) ? methodEntry->queryFunctionRegister() : nullptr;
    }
    virtual IEsdlTransformSet *queryMethodEntryPoint(const char *method, const char *name) override
    {
        IEsdlTransformEntryPointMap *epm = queryMethod(method);
        if (epm)
            return epm->queryEntryPoint(name);
        return nullptr;
    }

    virtual void removeMethod(const char *name) override
    {
        map.remove(name);
        if (isEmptyString(name))
        {
            service.setown(new CEsdlTransformEntryPointMap(nullptr));
            serviceFunctionRegister = service->queryFunctionRegister();
        }
    }
    virtual void addMethodTransforms(const char *method, const char *scriptXml, bool &foundNonLegacyTransforms) override
    {
        try
        {
            CEsdlTransformEntryPointMap *entry = map.getValue(method ? method : "");
            if (entry)
                entry->add(scriptXml, foundNonLegacyTransforms);
            else
            {
                //casting up from interface to known implementation, do it explicitly
                Owned<CEsdlTransformEntryPointMap> epm = new CEsdlTransformEntryPointMap(static_cast<CEsdlFunctionRegister*>(serviceFunctionRegister));
                epm->add(scriptXml, foundNonLegacyTransforms);
                map.setValue(method, epm.get());
            }
        }
        catch (XmlPullParserException& xppe)
        {
            VStringBuffer msg("Error parsing ESDL transform script (method '%s', line %d, col %d) %s", method ? method : "", xppe.getLineNumber(), xppe.getColumnNumber(), xppe.what());
            IERRLOG("%s", msg.str());
            throw MakeStringException(ESDL_SCRIPT_Error, "%s", msg.str());
        }
    }
    virtual void bindFunctionCalls() override
    {
        HashIterator it(map);
        ForEach (it)
        {
            const char *method = (const char *)it.query().getKey();
            if (isEmptyString(method)) //we validate the service level function calls against each method, not separately, (they are quasi virtual)
                continue;
            CEsdlTransformEntryPointMap *item = map.getValue(method);
            if (item)
            {
                item->bindFunctionCalls(method, item->queryFunctionRegister(), false);

                //service level function calls are resolved at runtime unless defined locally and called from the same script
                //  but we can issue a warning if they can't be resolved while handling the current method
                VStringBuffer serviceScopeDescr("service level at method %s", method);
                service->bindFunctionCalls(serviceScopeDescr, item->queryFunctionRegister(), true);
            }
        }
    }
};

esdl_decl IEsdlTransformMethodMap *createEsdlTransformMethodMap()
{
    return new CEsdlTransformMethodMap();
}
