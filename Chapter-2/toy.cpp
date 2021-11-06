#include <cstdlib>
#include <string>
#include <string.h>
#include <vector>
#include <cctype>
#include <iostream>

#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/IR/LegacyPassManager.h"
using namespace std;

enum Token_Type {
    EOF_TOKEN = 0,
    NUMERIC_TOKEN,    //数值类型
    IDENTIFIER_TOKEN, //标识符
    PARAN_TOKEN,      //括号
    DEF_TOKEN         //函数声明定义
};
//持有数值
static int Numeric_Val;
//持有字符串名称
static std::string Identifier_string;
FILE *file;

//Module_Ob包含了代码中的所有函数和变量
static llvm::Module *Module_Ob;
static llvm::LLVMContext TheContext;
//帮助生成LLVM IR并且记录程序的当前点，以插入LLVM指令
static llvm::IRBuilder<> Builder(TheContext);
//Named_Value 记录当前作用域中的所有已定义值，充当符号表的功能。对于toy来说，也包含函数参数信息
static std::map<std::string, llvm::Value*> Named_Values;
//静态变量管理函数
static llvm::legacy::FunctionPassManager *Global_FP;

//词法分析函数
static int get_token()
{
	//定义第一个字符，以防没有输入
    static int LastChar = ' ';
	//跳过空白字符（比如：空格、制表符(\t)、换行(\n)、回车等(\r））
    while (isspace(LastChar))
        LastChar = fgetc(file);
	//识别标识符
    if (isalpha(LastChar))
    {
        Identifier_string = LastChar;

        while (isalnum(LastChar = fgetc(file)))
            Identifier_string += LastChar;

        if (Identifier_string == "def") {
            //std::cout<<"string: "<<Identifier_string<<": "<<"DEF_TOKEN"<<"\n"; 
            return DEF_TOKEN;
        }
        //std::cout<<"string: "<<Identifier_string<<": "<<"IDENTIFIER_TOKEN"<<"\n";
        return IDENTIFIER_TOKEN;
    }
	//识别数值
    if (isdigit(LastChar))
    {
        std::string NumStr;
        do
        {
            NumStr += LastChar;
            LastChar = fgetc(file);
        } while (isdigit(LastChar));

        //string 转 int 
        Numeric_Val = strtod(NumStr.c_str(), 0);
        //std::cout<<"Numeric_Val: "<<Numeric_Val<<": "<<"NUMERIC_TOKEN"<<"\n";
        return NUMERIC_TOKEN;
    }
    //识别注释
    if(LastChar == '#')
    {
        do{
            LastChar = fgetc(file);
        }while(LastChar != EOF && LastChar != '\n' && LastChar != '\r');
        //非结束符 继续识别
        if(LastChar != EOF)
            return get_token();
    }
    if(LastChar == EOF) {
        //std::cout<<"EOF_TOKEN\n";
        return EOF_TOKEN;
    }
    //其他字符，如逗号，括号
    int ThisChar = LastChar;
    LastChar = fgetc(file);
    //std::cout<<"Other Cases: "<<(char)ThisChar<<": "<<"OTHERS_TYPE"<<"\n";
    return ThisChar;
}
//AST基类
class BaseAST
{
    public:
    virtual ~BaseAST(){};
    //表示静态单赋值（SSA）对象，返回值是LLVM Value对象
    virtual llvm::Value* Codegen() = 0;
};
//变量表达式的AST类定义
class VariableAST : public BaseAST
{
    //定义string对象来存储变量名
    std::string Var_Name;
    public:
    //VariableAST的含参构造函数
    VariableAST(std::string &name) : Var_Name(name){}
    virtual llvm::Value* Codegen();
};
//变量表达式的代码生成函数
llvm::Value *VariableAST::Codegen()
{
    llvm::Value *V = Named_Values[Var_Name];
    return V? V : 0;
}
//数值表达式的AST类定义
class NumericAST : public BaseAST
{
    int numeric_val;
    public:
    NumericAST(int val) : numeric_val(val) {}
    virtual llvm::Value *Codegen();
};
//数值变量的代码生成函数
llvm::Value *NumericAST::Codegen()
{
    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(TheContext), numeric_val);
}
//由二元运算符组成的表达式的AST类定义
class BinaryAST : public BaseAST
{
    //用来存储二元操作符的string对象
    std::string Bin_Operator;
    //用于存储一个二元表达式的 LHS 和 RHS 的对象，LHS 和 RHS 二元操作可以是任何类型，因此用 BaseAST 对象存储
    BaseAST *LHS, *RHS;
    public:
    //初始化二元操作符，LHS，RHS
    BinaryAST(std::string op, BaseAST *lhs, BaseAST *rhs): 
        Bin_Operator(op), LHS(lhs), RHS(rhs) {}
    virtual llvm::Value* Codegen();
};
//二元表达式的代码生成函数
llvm::Value *BinaryAST::Codegen()
{
    llvm::Value *L = LHS->Codegen();
    llvm::Value *R = RHS->Codegen();
    if(L == 0 || R == 0) return 0;
    //将C++中的 string 转为 C 中的字符
    switch(atoi(Bin_Operator.c_str()))
    {
        case '+': return Builder.CreateAdd(L, R, "addtmp");
        case '-': return Builder.CreateSub(L, R, "subtmp");
        case '*': return Builder.CreateMul(L, R, "multmp");
        case '/': return Builder.CreateUDiv(L, R, "divtmp");
        default: return 0;
    }
}
//函数声明的AST类定义
class FunctionDeclAST
{
    //函数声明名称
    std::string Func_Name;
    //函数声明的参数
    std::vector<std::string> Arguments;
    public:
    FunctionDeclAST(const std::string &name, const std::vector<std::string> &args):
        Func_Name(name), Arguments(args) {
    //cout<<"Initialize a FunctionDeclAST:\n";
    //cout<<"Func_Name: "<<Func_Name<<"\n";
    //for (auto arg : Arguments)
    //cout<<"arg: "<<arg<<"\n";
	}
    virtual llvm::Function* Codegen();
};
//函数定义的AST类定义
class FunctionDefnAST
{
    //函数定义的声明
    FunctionDeclAST *Func_Decl;
    //函数定义的函数体
    BaseAST* Body;
    public:
    FunctionDefnAST(FunctionDeclAST *proto, BaseAST *body) : Func_Decl(proto), Body(body){}
    virtual llvm::Function* Codegen();
};
//函数调用的AST类定义
class FunctionCallAST : public BaseAST
{
    
    //调用函数名
    std::string Function_Callee;
    //调用函数所含参数
    std::vector<BaseAST *> Function_Arguments;
    public:
    FunctionCallAST(const std::string &Callee, std::vector<BaseAST*> &args):
        Function_Callee(Callee), Function_Arguments(args){}
    virtual llvm::Value* Codegen();
};
//函数调用的代码生成函数
llvm::Value *FunctionCallAST::Codegen()
{
    //在之前已声明的函数中查找
    llvm::Function *CalleeF = Module_Ob->getFunction(Function_Callee);
    //llvm::errs()<<"Function:"<<*CalleeF<<"\n";
    //存储所传递参数的代码生成结果
    vector<llvm::Value*> ArgsV;
    for(unsigned i =0, e = Function_Arguments.size(); i != e; i++)
    {
        //调用所传递参数的Codegen（）函数
        ArgsV.push_back(Function_Arguments[i] -> Codegen());
        if(ArgsV.back() == 0) return 0;
    }
    //最后创建 LLVM调用指令
    return Builder.CreateCall(CalleeF, ArgsV, "calltemp");
}
//函数声明的代码生成函数
llvm::Function *FunctionDeclAST::Codegen()
{
    //std::cout<<"Inside FunctionDeclAST \n";
    vector<llvm::Type*> Integers(Arguments.size(), llvm::Type::getInt32Ty(TheContext));
    llvm::FunctionType *FT = llvm::FunctionType::get(llvm::Type::getInt32Ty(TheContext), Integers, false);
    llvm::Function *F = llvm::Function::Create(FT, llvm::Function::ExternalLinkage, Func_Name, Module_Ob);
	//std::cout<<"F->getName() : "<<F->getName().str()<<", Func_Name: "<<Func_Name<<"\n";
    if(F->getName().str() != Func_Name)
    {
        //std::cout<<"erase from parent.\n";
        F -> eraseFromParent();
        F = Module_Ob->getFunction(Func_Name);

        if(!F->empty())
            return 0;
        if(F -> arg_size() != Arguments.size())
            return 0;
    }
    unsigned Idx = 0;
    for(llvm::Function::arg_iterator Arg_It = F->arg_begin(); Idx != Arguments.size(); ++Arg_It, ++Idx)
    {
        Arg_It->setName(Arguments[Idx]);
        Named_Values[Arguments[Idx]] = Arg_It;
    }
    return F;
}
//函数定义的代码生成函数
llvm::Function *FunctionDefnAST::Codegen()
{
	//cout<<"Inside Function Definition AST Codegen : \n";
    Named_Values.clear();
    llvm::Function *TheFunction = Func_Decl->Codegen();
	//llvm::errs()<<"The Function: \n"<<*TheFunction<<"\n";
    if(TheFunction == 0) {
        //cout<<"TheFunction == 0.\n";
        return 0;
	}
    llvm::BasicBlock *BB = llvm::BasicBlock::Create(TheContext, "entry", TheFunction);
    Builder.SetInsertPoint(BB);

    if(llvm::Value *RetVal = Body->Codegen())
    {
        Builder.CreateRet(RetVal);
        verifyFunction(*TheFunction);
        Global_FP->run(*TheFunction);
        return TheFunction;
    }

    TheFunction->eraseFromParent();
    return 0;
}

//当前token（静态全局变量） 
static int Current_token;
//获取下一个token
static int next_token()
{
    Current_token = get_token();
    return Current_token;
}
//括号解析函数
static BaseAST* paran_parser();
static BaseAST* Base_Parser();
//二元运算符解析函数
static BaseAST* binary_op_parser(int, BaseAST *);
//数值表达式解析函数
static BaseAST *numeric_parser()
{
    BaseAST *Result = new NumericAST(Numeric_Val);
    next_token();
    return Result;
}
//表达式解析函数
static BaseAST* expression_parser()
{
	//cout<<"Inside expression_parser()\n";
    BaseAST *LHS = Base_Parser();
    if(!LHS) {
        //cout<<"!LHS\n";
        return 0;
	}
    return binary_op_parser(0, LHS);
}
//标识符解析函数
static BaseAST* identifier_parser()
{
    //cout<<"Inside identifier_parser()\n";
    std::string IdName = Identifier_string;
    next_token();
    if(Current_token != '(')
        return new VariableAST(IdName);

    next_token();

    std::vector<BaseAST*> Args;
    if(Current_token != ')')
    {
        while(1)
        {
            BaseAST *Arg = expression_parser();
            if(!Arg) return 0;
            Args.push_back(Arg);

            if(Current_token == ')') break;

            if(Current_token != ',')
                return 0;
            
            next_token();
        }
    }
    next_token();

    return new FunctionCallAST(IdName, Args);
}
//泛型函数，由当前token确定调用的解析函数
static BaseAST* Base_Parser()
{
	//cout<<"Inside Base_Parser()\n";
	//cout<<"Current_token:"<<Current_token<<"\n";
    switch(Current_token)
    {
        case IDENTIFIER_TOKEN : return identifier_parser();
        case NUMERIC_TOKEN : return numeric_parser();
        case '(' : return paran_parser();
        default: return 0;
    }
}
//函数声明解析函数
static FunctionDeclAST *func_decl_parser()
{
	//cout<<"Inside func_decl_parser():\n";
    if(Current_token != IDENTIFIER_TOKEN)
        return 0;
    
    std::string FnName = Identifier_string;
	//cout<<"FnName: "<<FnName<<"\n";
    next_token();

    if(Current_token != '(')
        return 0;

    std::vector<std::string> Function_Argument_Names;
    //原书代码如下，但未处理函数声明参数中，逗号的问题
    // while(next_token() == IDENTIFIER_TOKEN)
    // {
    //     Function_Argument_Names.push_back(Identifier_string);
    //     //cout<<"Identifier_string: "<<Identifier_string<<"\n";
    // }
    //我做出了如下修改：
    next_token();
    while( Current_token == IDENTIFIER_TOKEN || Current_token == ',')
    {
        if(Current_token == IDENTIFIER_TOKEN){
            Function_Argument_Names.push_back(Identifier_string);
            //cout<<"Identifier_string: "<<Identifier_string<<"\n";
            next_token();
        }
        if(Current_token == ','){
            next_token();
            continue;
        }
    }

    if(Current_token != ')')
        return 0;
    next_token();
    //cout<<"create FunctionDeclAST.\n";
    return new FunctionDeclAST(FnName, Function_Argument_Names);
}
//函数定义解析函数
static FunctionDefnAST *func_defn_parser()
{
	//cout<<"Inside function func_defn_parser():\n";
    next_token();
    FunctionDeclAST *Decl = func_decl_parser();
    if(Decl == 0)
        return 0;
    
    if(BaseAST *Body = expression_parser())
    {
        //cout<<"new FunctionDefnAST: \n";
        return new FunctionDefnAST(Decl, Body);
    }
    return 0;
}
//设置二元操作符优先级
static std::map<char, int> Operator_Precedence;
static void init_precedence()
{
    Operator_Precedence['-'] = 1;
    Operator_Precedence['+'] = 2;
    Operator_Precedence['/'] = 3;
    Operator_Precedence['*'] = 4;
}
//返回已定义的二元操作符优先级
static int getBinOpPrecedence()
{
    if (!isascii(Current_token))
        return -1;
    int TokPrec = Operator_Precedence[Current_token];
    if(TokPrec <= 0) return -1;

    return TokPrec;
}
//二元操作符的解析函数
static BaseAST* binary_op_parser(int Old_Prec, BaseAST *LHS)
{
	//cout<<"Inside binary_op_parser():\n";
    while(1)
    {
        //操作符优先级
        int Operator_Prec = getBinOpPrecedence();
        //cout<<"Operator_Prec :"<<Operator_Prec<<"\n";
        if(Operator_Prec < Old_Prec)
            return LHS;
        int BinOp = Current_token;
        //cout<<"BinOp : "<<(char)BinOp<<"\n";
        next_token();

        BaseAST* RHS = Base_Parser();
        if(!RHS) 
            return 0;

        int Next_Prec = getBinOpPrecedence();
        //cout<<"Next_Prec :"<<Next_Prec<<"\n";
        if(Operator_Prec < Next_Prec)
        {
            RHS = binary_op_parser(Operator_Prec + 1, RHS);
            if(RHS == 0)
                return 0;
        }
        LHS = new BinaryAST(std::to_string(BinOp), LHS, RHS);
    }
	//cout<<"End of binary_op_parser.\n";
}
static BaseAST* paran_parser()
{
    next_token();
    BaseAST* V = expression_parser();
    if(!V)
        return 0;
    
    if(Current_token != ')')
        return 0;
    return V;
}
static void HandleDefn()
{
    if(FunctionDefnAST *F = func_defn_parser())
    {
        if(llvm::Function *LF = F -> Codegen())
        {
    
        }
    }
    else
    {
        next_token();
    }
}
static FunctionDefnAST *top_level_parser()
{
	//cout<<"Inside top_level_parser()\n";
    if (BaseAST *E = expression_parser()) {
        FunctionDeclAST *Func_Decl = new FunctionDeclAST("", std::vector<std::string>());
        return new FunctionDefnAST(Func_Decl, E);
    }
    return 0;
}
static void HandleTopExpression()
{
	//cout<<"Inside HandleTopExpression():\n";
    if(FunctionDefnAST *F = top_level_parser())
    {
        if(llvm::Function *LF = F ->Codegen())
        {

        }
    }
    else
    {
        next_token();
    }
}
//驱动函数
static void Driver()
{
	//cout<<"Inside Driver :\n";
    while(1)
    {
        //printf("Current_token: %c \n", Current_token);
        //cout<<"Current_token : "<<Current_token<<"\n";
        switch(Current_token)
        {       
            case EOF_TOKEN: return;
            case ';': next_token(); break;
            //函数定义处理
            case DEF_TOKEN: {
                //std::cout<<"HandleDefn: \n";
                HandleDefn(); 
                break;
            }
            //表达式处理
            default: {
                //std::cout<<"handleTopExpr: \n";
                HandleTopExpression(); 
                break;
            }
        }
    }
}

int main(int argc, char *argv[])
{
    llvm::LLVMContext &Context = TheContext;
    init_precedence();
    file = fopen(argv[1], "r");
    if(file == 0)
    {
        printf("File not found.\n");
    }
    next_token();
    Module_Ob = new llvm::Module(argv[1], Context);
    //为Module定义一个函数管理器
    llvm::legacy::FunctionPassManager My_FP(Module_Ob);
    My_FP.add(llvm::createReassociatePass());
    My_FP.doInitialization();
    Global_FP = &My_FP;
    Driver();
    Module_Ob->print(llvm::outs(), nullptr);
    return 0;
}

