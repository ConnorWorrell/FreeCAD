/***************************************************************************
 *   Copyright (c) 2002 Jürgen Riegel <juergen.riegel@web.de>              *
 *   Copyright (c) 2014 Luke Parry <l.parry@warwick.ac.uk>                 *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/

#include "PreCompiled.h"

#ifndef _PreComp_
# include <sstream>
# include <QDomDocument>
# include <QDomNodeModel.h>
# include <QFile>
# include <QXmlQuery>
# include <QXmlResultItems>
#endif

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>     //TODO: should be in DrawTemplate.h??
#include <Base/Console.h>
#include <Base/FileInfo.h>
#include <Base/Quantity.h>
#include <Base/Tools.h>

#include "DrawPage.h"
#include "DrawSVGTemplate.h"
#include "DrawSVGTemplatePy.h"
#include "DrawUtil.h"


using namespace TechDraw;

PROPERTY_SOURCE(TechDraw::DrawSVGTemplate, TechDraw::DrawTemplate)

DrawSVGTemplate::DrawSVGTemplate()
{
    static const char *group = "Template";

    ADD_PROPERTY_TYPE(PageResult, (nullptr),  group, App::Prop_Output,    "Embedded SVG code for template. For system use.");   //n/a for users
    ADD_PROPERTY_TYPE(Template,   (""), group, App::Prop_None, "Template file name.");

    // Width and Height properties shouldn't be set by the user
    Height.setStatus(App::Property::ReadOnly, true);
    Width.setStatus(App::Property::ReadOnly, true);
    Orientation.setStatus(App::Property::ReadOnly, true);

    std::string svgFilter("Svg files (*.svg *.SVG);;All files (*)");
    Template.setFilter(svgFilter);
}

DrawSVGTemplate::~DrawSVGTemplate()
{
}

PyObject *DrawSVGTemplate::getPyObject()
{
    if (PythonObject.is(Py::_None())) {
        // ref counter is set to 1
        PythonObject = Py::Object(new DrawSVGTemplatePy(this), true);
    }
    return Py::new_reference_to(PythonObject);
}

void DrawSVGTemplate::onChanged(const App::Property* prop)
{
    if (prop == &Template && !isRestoring()) {
        //if we are restoring an existing file we just want the properties set as they were save,
        //but if we are not restoring, we need to replace the embedded file and extract the new
        //EditableTexts.
        //We could try to find matching field names are preserve the values from
        //the old template, but there is no guarantee that the same fields will be present.
        replaceFileIncluded(Template.getValue());
        EditableTexts.setValues(getEditableTextsFromTemplate());
    } else if (prop == &EditableTexts) {
        //handled by ViewProvider
    }

    TechDraw::DrawTemplate::onChanged(prop);
}

//parse the Svg code, inserting current EditableTexts values, and return the result as a QString.
//While parsing, note the Orientation, Width and Height values in the Svg code.
QString DrawSVGTemplate::processTemplate()
{
//    Base::Console().Message("DSVGT::processTemplate() - isRestoring: %d\n", isRestoring());
    if (isRestoring()) {
        //until everything is fully restored, the embedded file is not available, so we
        //can't do anything
        return QString();
    }

    QFile templateFile(Base::Tools::fromStdString(PageResult.getValue()));
    if (!templateFile.open(QIODevice::ReadOnly)) {
        Base::Console().Error("DrawSVGTemplate::processTemplate can't read embedded template %s!\n", PageResult.getValue());
        return QString();
    }

	QDomDocument templateDocument;
	if (!templateDocument.setContent(&templateFile)) {
        Base::Console().Error("DrawSVGTemplate::processTemplate - failed to parse file: %s\n",
            PageResult.getValue());
		return QString();
	}

	QXmlQuery query(QXmlQuery::XQuery10);
	QDomNodeModel model(query.namePool(), templateDocument);
	query.setFocus(QXmlItem(model.fromDomNode(templateDocument.documentElement())));

	// XPath query to select all <tspan> nodes whose <text> parent
	// has "freecad:editable" attribute
	query.setQuery(QString::fromUtf8(
		"declare default element namespace \"" SVG_NS_URI "\"; "
		"declare namespace freecad=\"" FREECAD_SVG_NS_URI "\"; "
		"//text[@freecad:editable]/tspan"));

	QXmlResultItems queryResult;
	query.evaluateTo(&queryResult);

	std::map<std::string, std::string> substitutions = EditableTexts.getValues();
	while (!queryResult.next().isNull())
	{
		QDomElement tspan = model.toDomNode(queryResult.current().toNodeModelIndex()).toElement();

		// Replace the editable text spans with new nodes holding actual values
		QString editableName = tspan.parentNode().toElement().attribute(QString::fromUtf8("freecad:editable"));
		std::map<std::string, std::string>::iterator item =
			substitutions.find(std::string(editableName.toUtf8().constData()));
		if (item != substitutions.end()) {
			// Keep all spaces in the text node
			tspan.setAttribute(QString::fromUtf8("xml:space"), QString::fromUtf8("preserve"));

			// Remove all child nodes and append text node with editable replacement as the only descendant
			while (!tspan.lastChild().isNull()) {
				tspan.removeChild(tspan.lastChild());
			}
			tspan.appendChild(templateDocument.createTextNode(QString::fromUtf8(item->second.c_str())));
		}
	}
    
    //Add font-family if not present
	query.setQuery(QString::fromUtf8(
		"declare default element namespace \"" SVG_NS_URI "\"; "
		"//text[@*]"));

	QRegExp fontFamilyRX(QString::fromUtf8("(?:font-family:)(.*)(?:;)"));
	QRegExp fontStretchRX(QString::fromUtf8("(?:font-stretch:)(.*)(?:;)"));
	QRegExp fontWeightRX(QString::fromUtf8("(?:font-weight:)(.*)(?:;)"));
	QRegExp fontSpecRX(QString::fromUtf8("(?:-inkscape-font-specification:)(.*)(?:;)"));
	QRegExp fontSizeRX(QString::fromUtf8("(?:font-size:)(.*)(?:;)"));
	QRegExp fontStyleRX(QString::fromUtf8("(?:font-style:)(.*)(?:;)"));
	fontFamilyRX.setMinimal(true);
	fontStretchRX.setMinimal(true);
	fontWeightRX.setMinimal(true);
	fontSpecRX.setMinimal(true);
	fontSizeRX.setMinimal(true);
	fontStyleRX.setMinimal(true);

	query.evaluateTo(&queryResult);
	while (!queryResult.next().isNull())
	{
		QDomElement textNode = model.toDomNode(queryResult.current().toNodeModelIndex()).toElement();
		QString textNodeStyle = textNode.toElement().attribute(QString::fromUtf8("style"));

		if (textNodeStyle.isEmpty()) continue;

		QString fontFamily = QString::fromUtf8("");
		QString fontSpec = QString::fromUtf8("");
		QString fontStyle = QString::fromUtf8("");
		QString fontWeight = QString::fromUtf8("");
		QString fontStretch = QString::fromUtf8("");
		QString fontSize = QString::fromUtf8("");

		//font Spec
		if(fontSpecRX.indexIn(textNodeStyle, 0)) {
			fontSpec = fontSpecRX.cap(1)
				.replace(fontFamily,QString::fromUtf8(""), Qt::CaseInsensitive)
				.replace(QString::fromUtf8("-"),QString::fromUtf8(""))
				.replace(QString::fromUtf8("'"),QString::fromUtf8(""));
		}

		//font Family
		if(fontFamilyRX.indexIn(textNodeStyle, 0)){
			fontFamily = fontFamilyRX.cap(1).replace(QString::fromUtf8("'"), QString::fromUtf8(""));
		}

		//font Weight
		if(fontWeightRX.indexIn(textNodeStyle, 0)) {
			fontWeight = fontWeightRX.cap(1).replace(QString::fromUtf8("-"),QString::fromUtf8(""));
		}

		//font Stretch
		if(fontStretchRX.indexIn(textNodeStyle, 0)) {
			fontStretch = fontStretchRX.cap(1).replace(QString::fromUtf8("-"),QString::fromUtf8(" "));
		}

		//font Size
		if(fontSizeRX.indexIn(textNodeStyle, 0)) {
			fontSize = fontSizeRX.cap(1);
		}

		//font style
		fontStyleRX.indexIn(textNodeStyle, 0);
		fontStyle = fontStyleRX.cap(1).replace(QString::fromUtf8("'"), QString::fromUtf8("")).replace(QString::fromUtf8("\""), QString::fromUtf8(""));

		//Search textNode node for missing information, extract it, and then delete the node.
		//This fixes the horizontal shift of the text
		if (!textNode.firstChild().isNull() && !textNode.firstChild().toElement().text().isEmpty()){
			QDomElement tspanNode = textNode.firstChild().toElement();
			QDomNode displayTextNode = textNode.firstChild().firstChild();

			QString displayTextNodeStyle = tspanNode.attribute(QString::fromUtf8("style"));
			if (!displayTextNodeStyle.isEmpty()){
				if (fontFamily.isEmpty()){
					fontFamilyRX.indexIn(displayTextNodeStyle, 0);
					fontFamily = fontFamilyRX.cap(1).replace(QString::fromUtf8("'"), QString::fromUtf8(""));
				}
				if (fontWeight.isEmpty()){
					fontWeightRX.indexIn(displayTextNodeStyle, 0);
					fontWeight = fontWeightRX.cap(1).replace(QString::fromUtf8("-"),QString::fromUtf8(""));
				}
				if (fontStretch.isEmpty()){
					fontStretchRX.indexIn(displayTextNodeStyle, 0);
					fontStretch = fontStretchRX.cap(1).replace(QString::fromUtf8("-"),QString::fromUtf8(" "));
				}
				if (fontSize.isEmpty()){
					fontSizeRX.indexIn(displayTextNodeStyle, 0);
					fontSize = fontSizeRX.cap(1);
				}
				if (fontStyle.isEmpty()){
					fontFamilyRX.indexIn(displayTextNodeStyle, 0);
					fontStyle = fontStyleRX.cap(1).replace(QString::fromUtf8("'"), QString::fromUtf8("")).replace(QString::fromUtf8("\""), QString::fromUtf8(""));
				}

			}

			textNode.appendChild(displayTextNode);
			textNode.removeChild(tspanNode);
		}

		if(fontSpec.isEmpty() || fontFamily.isEmpty() || fontStretch.isEmpty()) continue;

		//If font weight is a number, we must extract correct text from the fontSpec
		QString fontWeightForFontFamily = fontWeight;
		bool success;
		if (fontWeight.toInt(&success, 10) && success){
			fontWeightForFontFamily = fontSpec
				.replace(fontFamily, QString::fromUtf8("")).replace(QString::fromUtf8(" "), QString::fromUtf8(""), Qt::CaseInsensitive)
				.replace(fontStretch, QString::fromUtf8("")).replace(QString::fromUtf8(" "), QString::fromUtf8(""), Qt::CaseInsensitive);
		}
		if (fontStretch.toLower() == QString::fromUtf8("normal")){
			fontStretch = QString::fromUtf8("");
		}

		QString tempfontStretch = fontStretch;
		fontWeightForFontFamily = fontWeightForFontFamily.toLower().replace(fontStyle, QString::fromUtf8("")).replace(QString::fromUtf8(" "), QString::fromUtf8(""), Qt::CaseInsensitive)
			.replace(tempfontStretch.replace(QString::fromUtf8(" "), QString::fromUtf8("")), QString::fromUtf8("")).replace(QString::fromUtf8(" "), QString::fromUtf8(""), Qt::CaseInsensitive)
			.replace(QString::fromUtf8("Heavy"),QString::fromUtf8("Black"), Qt::CaseInsensitive)
			.replace(QString::fromUtf8("Ultra"),QString::fromUtf8("Extra"), Qt::CaseInsensitive);

		//Remove bold from font family if font stretch is not defined, in this case it is defined in font-weight attribute.
		if (fontWeightForFontFamily.toLower() == QString::fromUtf8("normal") || (fontStretch.isEmpty() && fontWeight == QString::fromUtf8("bold"))){
			fontWeightForFontFamily = QString::fromUtf8("");
		}

		//Build fontfamily string
		QString exportFontFamily = QString::fromUtf8("");
		if (!fontFamily.isEmpty()){
			exportFontFamily.append(fontFamily);
		}
		if (!fontStretch.isEmpty()){
			exportFontFamily.append(QString::fromUtf8(" "));
			exportFontFamily.append(fontStretch);
		}
		if (!fontWeightForFontFamily.isEmpty()){
			exportFontFamily.append(QString::fromUtf8(" "));
			exportFontFamily.append(fontWeightForFontFamily);
		}

		if (!exportFontFamily.isEmpty()) textNode.setAttribute(QString::fromUtf8("font-family"), exportFontFamily);
		if (!fontWeight.isEmpty()) textNode.setAttribute(QString::fromUtf8("font-weight"), fontWeight);
		if (!fontStyle.isEmpty()) textNode.setAttribute(QString::fromUtf8("font-style"), fontStyle);
		if (!fontSize.isEmpty()) textNode.setAttribute(QString::fromUtf8("font-size"), fontSize);
	}

	// Calculate the dimensions of the page and store for retrieval
	// Obtain the size of the SVG document by reading the document attributes
	QDomElement docElement = templateDocument.documentElement();
	Base::Quantity quantity;

	// Obtain the width
	QString str = docElement.attribute(QString::fromLatin1("width"));
	quantity = Base::Quantity::parse(str);
	quantity.setUnit(Base::Unit::Length);

	Width.setValue(quantity.getValue());

	str = docElement.attribute(QString::fromLatin1("height"));
	quantity = Base::Quantity::parse(str);
	quantity.setUnit(Base::Unit::Length);

	Height.setValue(quantity.getValue());

	bool isLandscape = getWidth() / getHeight() >= 1.;

	Orientation.setValue(isLandscape ? 1 : 0);

	//all Qt holds on files should be released on exit #4085
	return templateDocument.toString();
}

double DrawSVGTemplate::getWidth() const
{
    return Width.getValue();
}

double DrawSVGTemplate::getHeight() const
{
    return Height.getValue();
}

void DrawSVGTemplate::replaceFileIncluded(std::string newTemplateFileName)
{
//    Base::Console().Message("DSVGT::replaceFileIncluded(%s)\n", newTemplateFileName.c_str());
    if (newTemplateFileName.empty()) {
        return;
    }

    Base::FileInfo tfi(newTemplateFileName);
    if (tfi.isReadable()) {
        PageResult.setValue(newTemplateFileName.c_str());
    } else {
        throw Base::RuntimeError("Could not read the new template file");
    }
}

std::map<std::string, std::string> DrawSVGTemplate::getEditableTextsFromTemplate()
{
//    Base::Console().Message("DSVGT::getEditableTextsFromTemplate()\n");
    std::map<std::string, std::string> editables;

    std::string templateFilename = Template.getValue();
    if (templateFilename.empty()) {
        return editables;
    }

    Base::FileInfo tfi(templateFilename);
    if (!tfi.isReadable()) {
        // if there is a old absolute template file set use a redirect
        tfi.setFile(App::Application::getResourceDir() + "Mod/Drawing/Templates/" + tfi.fileName());
        // try the redirect
        if (!tfi.isReadable()) {
            Base::Console().Log("DrawSVGTemplate::getEditableTextsFromTemplate() not able to open %s!\n", Template.getValue());
            return editables;
        }
    }

    QFile templateFile(QString::fromUtf8(tfi.filePath().c_str()));
    if (!templateFile.open(QIODevice::ReadOnly)) {
        Base::Console().Log("DrawSVGTemplate::getEditableTextsFromTemplate() can't read template %s!\n", Template.getValue());
        return editables;
    }

    QDomDocument templateDocument;
    if (!templateDocument.setContent(&templateFile)) {
        Base::Console().Message("DrawSVGTemplate::getEditableTextsFromTemplate() - failed to parse file: %s\n",
                                Template.getValue());
        return editables;
    }

    QXmlQuery query(QXmlQuery::XQuery10);
    QDomNodeModel model(query.namePool(), templateDocument, true);
    query.setFocus(QXmlItem(model.fromDomNode(templateDocument.documentElement())));

    // XPath query to select all <tspan> nodes whose <text> parent
    // has "freecad:editable" attribute
    query.setQuery(QString::fromUtf8(
        "declare default element namespace \"" SVG_NS_URI "\"; "
        "declare namespace freecad=\"" FREECAD_SVG_NS_URI "\"; "
        "//text[@freecad:editable]/tspan"));

    QXmlResultItems queryResult;
    query.evaluateTo(&queryResult);

    while (!queryResult.next().isNull()) {
        QDomElement tspan = model.toDomNode(queryResult.current().toNodeModelIndex()).toElement();

        QString editableName = tspan.parentNode().toElement().attribute(QString::fromUtf8("freecad:editable"));
        QString editableValue = tspan.firstChild().nodeValue();

        editables[std::string(editableName.toUtf8().constData())] =
            std::string(editableValue.toUtf8().constData());
    }

    return editables;
}


// Python Template feature ---------------------------------------------------------
namespace App {
/// @cond DOXERR
PROPERTY_SOURCE_TEMPLATE(TechDraw::DrawSVGTemplatePython, TechDraw::DrawSVGTemplate)
template<> const char* TechDraw::DrawSVGTemplatePython::getViewProviderName() const {
    return "TechDrawGui::ViewProviderPython";
}
/// @endcond

// explicit template instantiation
template class TechDrawExport FeaturePythonT<TechDraw::DrawSVGTemplate>;
}
