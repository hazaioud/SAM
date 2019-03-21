#include <string>
#include <iostream>
#include <fstream>

#include <shared/lib_util.h>
#include <ssc/sscapi.h>

#include "lk_env.h"
#include "lk_eval.h"
#include "data_structures.h"
#include "variable_graph.h"
#include "ui_form_extractor.h"

#include "builder_generator.h"
#include "builder_generator_helper.h"

std::unordered_map<std::string, std::vector<std::string>> builder_generator::m_config_to_modules
    = std::unordered_map<std::string, std::vector<std::string>>();

std::unordered_map<std::string, std::unordered_map<std::string, callback_info>> builder_generator::m_config_to_callback_info
        = std::unordered_map<std::string, std::unordered_map<std::string, callback_info>>();

builder_generator::builder_generator(config_extractor *ce){
    config_ext = ce;
    config_name = config_ext->get_name();
    graph = config_ext->get_variable_graph();
    vertex* v = graph->find_vertex("d_rec", false);
    vertex* v2 = graph->find_vertex("d_rec", true);

    subgraph = new digraph(config_name);
    graph->subgraph_ssc_only(*subgraph);
    config_symbol = format_as_symbol(config_name);

    // load all cmod variables for this configuration and save the ssc types to the vertices
    auto cmods = SAM_config_to_primary_modules[active_config];
    for (size_t i = 0; i < cmods.size(); i++){
        std::string cmod_name = cmods[i];
        ssc_module_t p_mod = ssc_module_create(const_cast<char*>(cmod_name.c_str()));
        ssc_module_objects.insert({cmod_name, p_mod});

    }
}


vertex * builder_generator::var_user_defined(std::string var_name){
    vertex* v = subgraph->find_vertex(var_name, true);
    vertex* v2 = subgraph->find_vertex(var_name, false);
    if (v){
        // not user defined unless it's a source node
        int type = get_vertex_type(v);
        if (type != SOURCE && type != ISOLATED)
            return nullptr;
        else
            return v;
    }
    else if (v2){
        int type = get_vertex_type(v2);
        if (type != SOURCE && type != ISOLATED)
            return nullptr;
        else
            return v2;
    }
}

void builder_generator::select_ui_variables(std::string ui_name, std::map<std::string, vertex*>& var_map){
    std::unordered_map<std::string, VarValue> ui_def = SAM_ui_form_to_defaults[ui_name];
    for (auto it = ui_def.begin(); it != ui_def.end(); ++it){
        std::string var_name = it->first;
        VarValue* vv = &(it->second);

        if (vertex* v = var_user_defined(var_name)){
            v->ui_form = ui_name;
            var_map.insert({var_name, v});
        }
    }
}

void builder_generator::gather_variables(){
    // gather modules by input page
    std::vector<page_info>& pg_info = SAM_config_to_input_pages.find(active_config)->second;

    for (size_t p = 0; p < pg_info.size(); p++){
        if (pg_info[p].common_uiforms.size() > 0){
            // add the ui form variables into a group based on the sidebar title
            std::string group_name = pg_info[p].sidebar_title
                    + (pg_info[p].exclusive_uiforms.size() > 0 ? "Common" : "");
            std::map<std::string, vertex*> map;
            modules.insert({group_name, map});
            modules_order.push_back(group_name);
            std::map<std::string, vertex*>* var_map = &(modules.find(group_name)->second);

            for (size_t i = 0; i < pg_info[p].common_uiforms.size(); i++) {
                // add all the variables and associate their VarValue with the vertex
                std::string ui_name = pg_info[p].common_uiforms[i];
                select_ui_variables(ui_name, *var_map);
            }
        }

        // add each exclusive form as its own (sub)module
        for (size_t i = 0; i < pg_info[p].exclusive_uiforms.size(); i++) {
            // add all the variables and associate their VarValue with the vertex
            std::string ui_name = pg_info[p].exclusive_uiforms[i];

            std::string submod_name = ui_name;
            modules.insert({submod_name, std::map<std::string, vertex*>()});
            modules_order.push_back(submod_name);
            std::map<std::string, vertex*>& var_map = modules.find(submod_name)->second;

            select_ui_variables(ui_name, var_map);
        }
    }

    // add modules and inputs that are not in UI
    std::vector<std::string> primary_cmods = SAM_config_to_primary_modules[config_name];

    // extra modules first such as adjustments factors
    for (size_t i = 0; i < primary_cmods.size(); i++) {
        std::string cmod_name = primary_cmods[i];
        std::vector<std::string> map = cmod_to_extra_modules[cmod_name];
        for (size_t j = 0; j < map.size(); j++){
            std::string module_name = map[j];
            // add the module
            modules.insert({module_name, std::map<std::string, vertex*>()});
            modules_order.push_back(module_name);

            // add the variables
            auto extra_vars = extra_modules_to_members[module_name];
            auto module_map = modules[module_name];
            for (size_t k = 0; k < extra_vars.size(); j++){
                vertex* v = graph->add_vertex(extra_vars[k], true);
                v->cmod = cmod_name;
                module_map.insert({extra_vars[k], v});
            }
        }

        // add an extra "common" module to catch unsorted variables
        modules.insert({"Common", std::map<std::string, vertex*>()});
        modules_order.push_back("Common");

        // go through all the ssc variables and make sure they are in the graph and in modules
        std::vector<std::string> all_ssc = SAM_cmod_to_inputs[cmod_name];
        for (size_t j = 0; j < all_ssc.size(); j++){
            std::string var = all_ssc[j];
            vertex* v = graph->find_vertex(var, true);
            if (!v){
                // see if it belongs to a module
                std::string md = find_module_of_var(var, cmod_name);

                v = graph->add_vertex(var, true);
                v->cmod = cmod_name;
                if (md.length() != 0){
                    modules.find(md)->second.insert({var, v});
                }
                // add it to "common"
                else{
                    modules.find("Common")->second.insert({var, v});
                }
            }
        }

        // delete Common if it's empty
        if (modules["Common"].size() == 0)
            modules.erase("Common");
    }

    m_config_to_modules.insert({config_name, modules_order});
}


//
void builder_generator::export_variables_json(const std::string &cmod) {
    std::ofstream json;
    json.open(filepath + "/defaults/" + cmod + ".json");

    // later implement for several financial models
    std::string financial = config_name.substr(config_name.find('-')+1);
    assert(json.is_open());

    json << "{\n";
    json << "\t\"" + cmod + "_defaults\": {\n";

    std::unordered_map<std::string, bool> completed_tables;
    for (size_t i = 0; i < modules_order.size(); i++){
        std::string module_name = modules_order[i];
        json << "\t\t\"" + module_name + "\": {\n";

        std::map<std::string, vertex*>& map = modules.find(module_name)->second;
        for (auto it = map.begin(); it != map.end(); ++it){
            std::string var = it->first;
            vertex* v = it->second;

            int vv_type = get_varvalue_type(v->name, config_name);

            // if it's a table entry, print the whole table rather than single entry
            if(var.find(":") != std::string::npos){
                var = var.substr(0, var.find(":"));
                vv_type = SSC_TABLE;
                if (completed_tables.find(var) == completed_tables.end()){
                    completed_tables.insert({var, true});
                }
                else{
                    continue;
                }
            }

            std::vector<std::string> vv_typestr = {"undefined", "float", "array"
                    , "matrix", "string", "var_table", "binary"};

            json << "\t\t\t\"" + var + "\": {\n";
            json << "\t\t\t\t\"type\": \"" << vv_typestr[vv_type] << "\",\n";
            json << "\t\t\t\t\"" << financial << "\": ";

            VarValue* vv = SAM_config_to_defaults[config_name][var];

            // vv can be null in the case of variables not available in UI
            json << ssc_value_to_json(vv_type, vv) << "\n\t\t\t}";

            if (std::next(it) != map.end()) json << ",";
            json << "\n";
        }
        json << "\t\t}";
        if (i != modules_order.size() - 1) json << ",";
        json << "\n";
    }
    json << "\t}\n}";

    json.close();

}


std::unordered_map<std::string, edge *> builder_generator::gather_functions() {
    std::unordered_map<std::string, edge*> unique_subgraph_edges;
    subgraph->get_unique_edge_expressions(unique_subgraph_edges);


    // group all the edges from the same LK object together
    std::unordered_map<std::string, std::unique_ptr<digraph>> fx_object_graphs;

    auto vertices = graph->get_vertices();
    for (auto it = vertices.begin(); it != vertices.end(); ++it){
        for (size_t is_ssc = 0; is_ssc < 2; is_ssc++){
            vertex* v = it->second.at(is_ssc);
            if (!v)
                continue;

            for (size_t i = 0; i < v->edges_out.size(); i++){
                edge* e = v->edges_out[i];
                auto edge_grp = fx_object_graphs.find(e->obj_name);
                if ( edge_grp == fx_object_graphs.end()){
                    std::unique_ptr<digraph> graph(new digraph(config_name));
                    vertex* src = graph->add_vertex(e->src->name, e->src->is_ssc_var, e->ui_form);
                    vertex* dest = graph->add_vertex(e->dest->name, e->dest->is_ssc_var, e->ui_form);
                    graph->add_edge(src, dest, e->type, e->obj_name, e->expression, e->ui_form, e->root);
                    fx_object_graphs.insert({e->obj_name, std::move(graph)});
                }
                else{
                    vertex* s = edge_grp->second->add_vertex(e->src->name, e->src->is_ssc_var, e->ui_form);
                    vertex* d = edge_grp->second->add_vertex(e->dest->name, e->dest->is_ssc_var, e->ui_form);
                    edge_grp->second->add_edge(s, d, e->type, e->obj_name, e->expression, e->ui_form, e->root);
                }
            }
        }
    }

    // for edges in subgraph, sort information in equation_info and callback_info
    for (auto it = unique_subgraph_edges.begin(); it != unique_subgraph_edges.end(); ++it){

        edge* e = it->second;

        // for equations, all the inputs and outputs are stored in equation_info
        if (e->type == 0){

            equation_info& eq_info = find_equation_info_from_edge(e, config_name);

            for (size_t k = 0; k < eq_info.all_inputs.size(); k++){
                std::string name = eq_info.all_inputs[k];
                if (which_cmod_as_input(name, config_name).length() > 0)
                    eq_info.ssc_only_inputs.push_back(name);
                else{
                    eq_info.ui_only_inputs.push_back(name);

                }
            }

            for (size_t k = 0; k < eq_info.ui_only_inputs.size(); k++){
                for (size_t m = 0; m < eq_info.all_outputs.size(); m++){
                    vertex* src = subgraph->add_vertex(eq_info.ui_only_inputs[k], false, e->ui_form);
                    vertex* dest = subgraph->find_vertex(eq_info.all_outputs[m], true);
                    subgraph->add_edge(src, dest, e->type, e->obj_name, e->expression, e->ui_form, e->root);
                }
            }
        }
        // for callbacks, need to find inputs and outputs from the original graph
        else{
            auto group_graph = fx_object_graphs.find(it->first);
            assert(group_graph != fx_object_graphs.end());

            // add a new callback_info to the map
            callback_info cb_info;

            auto vec_it = m_config_to_callback_info.find(config_name);
            if (vec_it == m_config_to_callback_info.end())
                m_config_to_callback_info.insert({config_name, std::unordered_map<std::string, callback_info>()});
            auto& cb_info_vec = m_config_to_callback_info.find(config_name)->second;


            // get input/output info for callback_info, rest will be filled in during translation
            auto vertices = group_graph->second->get_vertices();

            // sort the vertices into inputs and outputs
            for (auto v_it = vertices.begin(); v_it != vertices.end(); ++v_it){
                for (size_t is_ssc = 0; is_ssc < 2; is_ssc++){
                    vertex* v = v_it->second.at(is_ssc);
                    if (!v)
                        continue;
                    int type = get_vertex_type(v);

                    // if it is another cmod, add all the ui inputs of that cmod
                    auto it = SAM_cmod_to_inputs.find(v->name);
                    if (it != SAM_cmod_to_inputs.end()) {
                        for (size_t i = 0; i < it->second.size(); i++) {
                            cb_info.ui_only_inputs.push_back(it->second[i]);
                            // add new inputs and connect it only to the secondary cmod
                            vertex* src = subgraph->add_vertex(it->second[i], false, e->ui_form);
                            src->cmod = v->name;

                            vertex* dest = subgraph->find_vertex(v->name, false);
                            dest->cmod = v->name;
                            subgraph->add_edge(src, dest, e->type, e->obj_name, e->expression, e->ui_form, e->root);

                            // add to original graph also
                            src = graph->add_vertex(it->second[i], false, e->ui_form);
                            src->cmod = v->name;
                            dest = graph->find_vertex(v->name, false);
                            dest->cmod = v->name;
                            graph->add_edge(src, dest, e->type, e->obj_name, e->expression, e->ui_form, e->root);
                        }
                        continue;
                    }

                    // if it's an input
                    if (type == SOURCE){
                        if (is_ssc)
                            cb_info.ssc_only_inputs.push_back(v->name);
                        // might be name of secondary compute module
                        else{
                            // add new input and copy its edges from original graph
                            cb_info.ui_only_inputs.push_back(v->name);
                            vertex* src = subgraph->add_vertex(v->name, false);

                            vertex* src_og = graph->find_vertex(v->name, false);
                            assert(src_og);
                            for (size_t m = 0; m < src_og->edges_out.size(); m++){
                                vertex* dest_og = src_og->edges_out[m]->dest;
                                vertex* dest = subgraph->add_vertex(dest_og->name, dest_og->is_ssc_var, dest_og->ui_form);

                                subgraph->add_edge(src, dest, e->type, e->obj_name, e->expression, e->ui_form, e->root);
                            }
                        }
                    }
                    else if (type == SINK){
                        cb_info.all_outputs.push_back(v->name);
                    }
                }
            }

            cb_info_vec.insert({e->obj_name, cb_info});

        }
    }

    return unique_subgraph_edges;
}


void builder_generator::create_SAM_headers(std::string cmod_name, std::string module, std::ofstream &fx_file) {
    std::string module_symbol = format_as_symbol(module);

    std::string sig = "SAM_" + format_as_symbol(cmod_name) + "_" + module_symbol;

    // create the module-specific var_table wrapper

    fx_file << "\t/** \n";
    fx_file << "\t * Create a " << module_symbol << " variable table for a " << config_symbol << " system\n";
    fx_file << "\t * @param def: the set of financial model-dependent defaults to use (None, Residential, ...)\n";
    fx_file << "\t * @param[in,out] err: a pointer to an error object\n";
    fx_file << "\t */\n";

    export_function_declaration(fx_file, sig, sig + "_create", {"const char* def"});

    fx_file << "\n";

    // setters
    std::map<std::string, vertex*> ssc_vars = modules[module];
    std::vector<vertex*> interface_vertices;
    for (auto it = ssc_vars.begin(); it != ssc_vars.end(); ++it){
        std::string var_name = it->first;

        vertex* v = it->second;

        interface_vertices.push_back(v);

        std::string var_symbol = sig + "_" + var_name;

        int ind = (int)SAM_cmod_to_ssc_index[cmod_name][var_name];
        ssc_info_t mod_info = ssc_module_var_info(ssc_module_objects[cmod_name], ind);

        fx_file << "\t/**\n";
        fx_file << "\t * Set " << var_name << ": " << ssc_info_label(mod_info) << "\n";
        fx_file << "\t * type: " << spell_type(ssc_info_data_type(mod_info)) << "\n";
        fx_file << "\t * units: ";
        std::string units_str = ssc_info_units(mod_info);
        if (units_str.length() > 0)
            fx_file << units_str << "\n";
        else
            fx_file << "None\n";

        fx_file << "\t * options: ";
        std::string meta_str = ssc_info_meta(mod_info);
        if (meta_str.length() > 0)
            fx_file << meta_str << "\n";
        else
            fx_file << "None\n";

        fx_file << "\t * constraints: ";
        std::string cons_str = ssc_info_constraints(mod_info);
        if (cons_str.length() > 0)
            fx_file << cons_str << "\n";
        else
            fx_file << "None\n";

        fx_file << "\t * required if: ";
        std::string req_str = ssc_info_required(mod_info);
        if (req_str.find('=') != std::string::npos)
            fx_file << req_str << "\n";
        else
            fx_file << "None\n";
        fx_file << "\t */\n";

        export_function_declaration(fx_file, "void", var_symbol + "_set", {sig + " ptr",
                                                                           print_parameter_type(v, cmod_name,
                                                                                                ssc_module_objects)});
    }

    // getters
    fx_file << "\n\t/**\n";
    fx_file << "\t * Getters\n\t */\n\n";

    for (size_t i = 0; i < interface_vertices.size(); i++){
        vertex* v = interface_vertices[i];
        std::string var_symbol = sig + "_" + v->name;

        export_function_declaration(fx_file, print_return_type(v, cmod_name, ssc_module_objects), var_symbol + "_get", {sig + " ptr"});
    }
}

void builder_generator::create_api_header(std::string cmod_name) {
    std::string cmod_symbol = format_as_symbol(cmod_name);

    // declaration of modules and submodules by group
    std::ofstream fx_file;
    fx_file.open(filepath + "/" + cmod_symbol + "-builder.h");
    assert(fx_file.is_open());

    fx_file << "#ifndef SAM_" << util::upper_case(cmod_symbol) << "_FUNCTIONS_H_\n";
    fx_file << "#define SAM_" << util::upper_case(cmod_symbol) << "_FUNCTIONS_H_\n\n";
    fx_file << "#include \"" << cmod_symbol << "-data.h\"\n\n";

    const char* includes = "#include <stdint.h>\n"
                           "#ifdef __cplusplus\n"
                           "extern \"C\"\n"
                           "{\n"
                           "#endif\n\n";

    fx_file << includes;

    for (size_t i = 0; i < modules_order.size(); i++){
        if (modules[modules_order[i]].size() == 0){
            continue;
        }
        std::string module_name = modules_order[i];
        create_SAM_headers(cmod_name, module_name, fx_file);
        fx_file << "\n\n";
    }

    const char* footer = "#ifdef __cplusplus\n"
                         "} /* end of extern \"C\" { */\n"
                         "#endif\n\n"
                         "#endif";

    fx_file << footer;
    fx_file.close();
}



bool builder_generator::eqn_in_subgraph(equation_info eq){
    for (size_t i = 0; i < eq.all_inputs.size(); i++ ){
        std::string name = eq.all_inputs[i];
        bool is_ssc = which_cmod_as_input(name, config_name).length() > 0;
        if (!is_ssc && !subgraph->find_vertex(eq.all_inputs[i], false))
            return false;
    }
    for (size_t i = 0; i < eq.all_outputs.size(); i++ ){
        std::string name = eq.all_outputs[i];
        bool is_ssc = which_cmod_as_input(name, config_name).length() > 0;
        if (is_ssc)
            return true;
        if (!is_ssc && !subgraph->find_vertex(eq.all_outputs[i], false))
            return false;
    }
    return true;
}

void builder_generator::create_cmod_builder_cpp(std::string cmod_name,
                                                const std::unordered_map<std::string, edge *> &unique_edge_obj_names) {
    // open header file
    std::ofstream header_file;
    header_file.open(filepath + "/cmod_" +  cmod_name + "-builder.h");
    assert(header_file.is_open());

    header_file << "#ifndef _CMOD_" << util::upper_case(cmod_name) << "_BUILDER_H_\n";
    header_file << "#define _CMOD_" << util::upper_case(cmod_name) << "_BUILDER_H_\n";

    const char* include_h = "\n"
                            "#include \"vartab.h\"";

    header_file << include_h << "\n\n\n";

    // open cpp file
    std::ofstream cpp_file;
    std::string cmod_symbol = format_as_symbol(cmod_name);

    cpp_file.open(filepath + "/cmod_" +  cmod_name + "-builder.cpp");
    assert(cpp_file.is_open());


    const char* include_cpp = "#include <string>\n"
                           "#include <vector>\n\n"
                           "#include \"vartab.h\"\n\n";

    cpp_file << include_cpp;
    cpp_file << "#include \"cmod_" << cmod_name << "-builder.h\"\n\n";

    std::string sig = "SAM_" + cmod_symbol;


    int number_functions_printed = 0;

    for (auto it = unique_edge_obj_names.begin(); it != unique_edge_obj_names.end(); ++it){
        edge* e = it->second;
        // translate equations
        if (e->type == 0){

            equation_info& eq_info = find_equation_info_from_edge(e, config_name);
            assert(eq_info.eqn_data->tree);

            try {
                if (config_ext->completed_equation_signatures.find(&eq_info)
                        == config_ext->completed_equation_signatures.end()){
                    translate_equation_to_cplusplus(config_ext, eq_info, cpp_file, cmod_name);
                }
            }
            catch (std::exception& e){
                std::cout << e.what() << "\n";
            }
            cpp_file << "\n\n";

            std::string fx_sig = config_ext->completed_equation_signatures[&eq_info];

            std::cout << e->ui_form << fx_sig;


            // make a nice comment block
            header_file << "//\n// Evaluates ";
            for (size_t k = 0; k < eq_info.all_outputs.size(); k++){
                header_file << eq_info.all_outputs[k];
                if (k != eq_info.all_outputs.size() - 1)
                    header_file << ", ";
            }
            header_file << " for a " << e->ui_form << " module\n";
            header_file << "// @param *vt: a var_table* that contains: ";
            for (size_t k = 0; k < eq_info.all_inputs.size(); k++){
                header_file << eq_info.all_inputs[k];
                if (k != eq_info.all_inputs.size() - 1)
                    header_file << ", ";
                else header_file << "\n";
            }
            header_file << "// @returns single value or var_table\n//\n";
            header_file << fx_sig << ";\n\n";

        }
        // translate callbacks
        else{
            std::string obj_name = e->obj_name;

            size_t pos = e->obj_name.find(":");
            if (pos != std::string::npos){
                std::string s = e->obj_name.substr(pos + 1);
                if (s != "MIMO")
                    obj_name = e->obj_name.substr(0, pos);
                else
                    obj_name = s;
            }

            std::vector<std::string> method_names = {"", "on_load", "on_change"};

            callback_info& cb_info = m_config_to_callback_info.find(config_name)->second.find(e->obj_name)->second;

            assert(cb_info.ui_only_inputs.size() + cb_info.ssc_only_inputs.size () > 0);

            cb_info.function_name = obj_name;
            cb_info.method_name = method_names[e->type];
            cb_info.ui_source = find_ui_of_object(obj_name, config_name)->ui_form_name;

            try {
                if (config_ext->completed_callback_signatures.find(&cb_info)
                    == config_ext->completed_callback_signatures.end()){
                    translate_callback_to_cplusplus(config_ext, cb_info, cpp_file, cmod_name);
                }
            }
            catch (std::exception& e){
                std::cout << e.what() << "\n";
            }

            cpp_file << "\n\n";

            std::string fx_sig = config_ext->completed_callback_signatures[&cb_info];

            std::cout << e->ui_form << fx_sig;

            // insert default invoke_t* cxt = 0 for header
            pos = fx_sig.find("cxt");
            assert(pos != std::string::npos);
            fx_sig.insert(pos+3, " = 0");

            // make a nice comment block
            header_file << "//\n// Function ";
            for (size_t k = 0; k < cb_info.all_outputs.size(); k++){
                header_file << cb_info.all_outputs[k];
                if (k != cb_info.all_outputs.size() - 1)
                    header_file << ", ";
            }
            header_file << " for a " << e->ui_form << " module\n";
            header_file << "// @param *vt: a var_table* that contains: ";
            for (size_t k = 0; k < cb_info.ssc_only_inputs.size(); k++){
                header_file << cb_info.ssc_only_inputs[k];
                if (k != cb_info.ssc_only_inputs.size() - 1)
                    header_file << ", ";
                else header_file << "\n";
            }
            for (size_t k = 0; k < cb_info.ui_only_inputs.size(); k++){
                header_file << cb_info.ui_only_inputs[k];
                if (k != cb_info.ui_only_inputs.size() - 1)
                    header_file << ", ";
                else header_file << "\n";
            }
            header_file << "// @param[in,out] *cxt: a invoke_t* that for storing the results\n";
            header_file << "// @returns single value or var_table\n//\n";
            header_file << fx_sig << "\n\n";
        }
    }

    std::cout << number_functions_printed << "\n";
    cpp_file.close();


    header_file << "#endif";
    header_file.close();
}


std::vector<std::string> builder_generator::get_user_defined_variables(){
    std::vector<std::string> vec;
    for (auto it = modules.begin(); it != modules.end(); ++it){
        for (auto it2 = it->second.begin(); it2 != it->second.end(); ++it2){
            vec.push_back(it2->second->name);
        }
    }
    return vec;
}

std::vector<std::string> builder_generator::get_evaluated_variables() {
    std::vector<std::string> vec;
    auto vertices = subgraph->get_vertices();
    for (auto it = vertices.begin(); it != vertices.end(); ++it){
        if (vertex* v = it->second.at(true)){
            if (get_vertex_type(v) != SOURCE)
                vec.push_back(v->name);
        }
    }
    return vec;
}


void builder_generator::create_all(std::string fp) {
    filepath = fp;

    // gather functions before variables to add in ui-only variables that may be skipped in subgraph
    std::unordered_map<std::string, edge*> unique_subgraph_edges = gather_functions();

    // epand the subgraph to include ui variables which may affect downstream ssc variables
    graph->subgraph_ssc_to_ui(*subgraph);

    std::vector<std::string> primary_cmods = SAM_config_to_primary_modules[config_name];


    // only working on technology systems, cannot yet pair with financial model
    // modules and modules_order will need to be reset per cmod
    if(primary_cmods.size() != 1){
        std::cout << "warning: really not implemented yet but short circuit for now\n;";
    }

    gather_variables();




        create_api_header(primary_cmods[0]);
        create_cmod_builder_cpp(primary_cmods[0], unique_subgraph_edges);

    // export defaults for all configurations at the end
    export_variables_json(primary_cmods[0]);


    auto udv = get_user_defined_variables();

    auto evalv = get_evaluated_variables();

    size_t all_ssc_vars = 0;

    auto cmods = SAM_config_to_primary_modules[config_name];

    auto all_vars = udv;

    for (size_t i = 0; i < cmods.size(); i++){
        auto vec = get_cmod_var_info(cmods[i], "in");
        all_vars = vec;
        all_ssc_vars += vec.size();
    }

    std::cout << config_name << ": \n";
    std::cout << "number user defined: " << udv.size() << "; number eval: " << evalv.size() << "\n";
    std::cout << "number of all ssc vars: " << all_ssc_vars << " in " << cmods.size() << " cmods\n";

    std::cout << udv << "\n" << evalv << "\n\n\n" << all_vars;

}



